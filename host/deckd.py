#!/usr/bin/env python3
"""deckd — sends the cyberdeck status to the CyberCYDeck screen (CYD) over USB serial.

Runs on the deck's Linux board (Orange Pi Zero 2W, Raspberry Pi...). Every few seconds it
writes one line to the CYD:  deck {json}   (format: docs/deck-protocol.md)

Sources, all optional except the basic system stats:
  - CPU, RAM, temperature, uptime: /proc and /sys
  - Wi-Fi: /proc/net/wireless + `ip`
  - battery: /sys/class/power_supply (UPS boards with a driver) or an INA219 on I2C (smbus2)
  - 4G: ModemManager (`mmcli -J`)
  - GPS: gpsd on localhost:2947
  - Meshtastic: a node on USB, through the `meshtastic` Python package

Usage:  deckd.py [--config /etc/deckd.ini] [--stdout] [--once]
"""
import argparse
import configparser
import glob
import json
import os
import socket
import subprocess
import sys
import threading
import time

DEFAULTS = {
    "deck": {"port": "auto", "interval": "3", "name": socket.gethostname()},
    "battery": {"source": "auto", "ina219_bus": "1", "ina219_addr": "0x40", "cells": "2",
                "v_empty": "3.30", "v_full": "4.15", "low_pct": "15"},
    "lte": {"enabled": "auto"},
    "gps": {"enabled": "auto"},
    "mesh": {"port": "", "max_msgs": "3"},
}

CH340 = "1a86:7523"   # USB-serial chip on the ESP32-2432S028


def log(*a):
    print("[deckd]", *a, file=sys.stderr, flush=True)


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def run(cmd, timeout=4):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


# ---------- system ----------
_cpu_prev = None


def cpu_pct():
    global _cpu_prev
    f = read("/proc/stat").splitlines()
    if not f:
        return -1
    v = [int(x) for x in f[0].split()[1:]]
    idle, total = v[3] + (v[4] if len(v) > 4 else 0), sum(v)
    prev, _cpu_prev = _cpu_prev, (idle, total)
    if not prev or total == prev[1]:
        return -1
    return round(100 * (1 - (idle - prev[0]) / (total - prev[1])))


def ram_pct():
    m = {}
    for line in read("/proc/meminfo").splitlines():
        k, _, v = line.partition(":")
        m[k] = int(v.split()[0]) if v.split() else 0
    if not m.get("MemTotal"):
        return -1
    return round(100 * (1 - m.get("MemAvailable", 0) / m["MemTotal"]))


def temp_c():
    vals = [int(read(p, "0")) for p in glob.glob("/sys/class/thermal/thermal_zone*/temp")]
    vals = [v for v in vals if v > 0]
    return round(max(vals) / 1000) if vals else -1


def uptime_s():
    u = read("/proc/uptime").split()
    return int(float(u[0])) if u else 0


# ---------- wi-fi ----------
def wifi():
    out = {}
    lines = read("/proc/net/wireless").splitlines()[2:]
    if not lines:
        return out
    iface = lines[0].split(":")[0].strip()
    try:
        out["rssi"] = int(float(lines[0].split()[3]))
    except (IndexError, ValueError):
        pass
    ssid = run(["iwgetid", "-r", iface]).strip()
    if not ssid:
        for line in run(["iw", "dev", iface, "link"]).splitlines():
            if line.strip().startswith("SSID:"):
                ssid = line.split(":", 1)[1].strip()
    if ssid:
        out["ssid"] = ssid
    for line in run(["ip", "-4", "-o", "addr", "show", iface]).splitlines():
        parts = line.split()
        if "inet" in parts:
            out["ip"] = parts[parts.index("inet") + 1].split("/")[0]
    return out


# ---------- battery ----------
def pct_from_volts(v, cfg):
    cells = cfg.getint("battery", "cells")
    lo, hi = cfg.getfloat("battery", "v_empty") * cells, cfg.getfloat("battery", "v_full") * cells
    return max(0, min(100, round(100 * (v - lo) / (hi - lo))))


def battery_sysfs():
    for d in glob.glob("/sys/class/power_supply/*"):
        if read(d + "/type") != "Battery":
            continue
        b = {}
        cap = read(d + "/capacity")
        if cap.isdigit():
            b["pct"] = int(cap)
        uv = read(d + "/voltage_now")
        if uv.lstrip("-").isdigit():
            b["v"] = round(int(uv) / 1e6, 2)
        b["chg"] = read(d + "/status") in ("Charging", "Full")
        if "pct" in b:
            return b
    return None


def battery_ina219(cfg):
    try:
        from smbus2 import SMBus
    except ImportError:
        return None
    addr = int(cfg.get("battery", "ina219_addr"), 0)
    try:
        with SMBus(cfg.getint("battery", "ina219_bus")) as bus:
            raw = bus.read_word_data(addr, 0x02)            # bus voltage register
            raw = ((raw & 0xFF) << 8) | (raw >> 8)           # INA219 is big endian
            v = (raw >> 3) * 0.004
            sh = bus.read_word_data(addr, 0x01)              # shunt voltage register
            sh = ((sh & 0xFF) << 8) | (sh >> 8)
            if sh & 0x8000:
                sh -= 0x10000
    except OSError:
        return None
    # current flowing into the battery (positive shunt voltage) means charging; depends on wiring
    return {"pct": pct_from_volts(v, cfg), "v": round(v, 2), "chg": sh > 50}


def battery(cfg):
    src = cfg.get("battery", "source")
    if src in ("auto", "sysfs"):
        b = battery_sysfs()
        if b or src == "sysfs":
            return b
    if src in ("auto", "ina219"):
        return battery_ina219(cfg)
    return None


# ---------- 4G (ModemManager) ----------
def lte(cfg):
    if cfg.get("lte", "enabled") == "no":
        return None
    lst = run(["mmcli", "-L", "-J"])
    try:
        paths = json.loads(lst).get("modem-list", [])
    except ValueError:
        return None
    if not paths:
        return None
    try:
        m = json.loads(run(["mmcli", "-m", paths[0].rsplit("/", 1)[-1], "-J"])).get("modem", {})
    except ValueError:
        return None
    out = {"op": m.get("3gpp", {}).get("operator-name", "") or "sem operadora"}
    sig = m.get("generic", {}).get("signal-quality", {}).get("value")
    if str(sig).isdigit():
        out["sig"] = int(sig)
    return out


# ---------- GPS (gpsd) ----------
def gps(cfg):
    if cfg.get("gps", "enabled") == "no":
        return None
    try:
        s = socket.create_connection(("127.0.0.1", 2947), timeout=1.5)
    except OSError:
        return None
    fix, sats, buf, end = False, -1, b"", time.time() + 2.5
    try:
        s.sendall(b'?WATCH={"enable":true,"json":true}\n')
        s.settimeout(0.5)
        while time.time() < end and (sats < 0 or not fix):
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for line in lines:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if o.get("class") == "TPV":
                    fix = o.get("mode", 0) >= 2
                elif o.get("class") == "SKY":
                    sats = o.get("uSat", sum(1 for x in o.get("satellites", []) if x.get("used")))
    finally:
        s.close()
    return {"fix": fix, "sats": max(sats, 0)}


# ---------- Meshtastic ----------
class Mesh:
    def __init__(self, port, max_msgs):
        self.port, self.max_msgs = port, max_msgs
        self.iface, self.msgs, self.lock = None, [], threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            import meshtastic.serial_interface
            from pubsub import pub
        except ImportError:
            log("pacote meshtastic não instalado (pip install meshtastic); mesh desligado")
            return
        pub.subscribe(self._on_text, "meshtastic.receive.text")
        while True:
            try:
                self.iface = meshtastic.serial_interface.SerialInterface(devPath=self.port)
                log("meshtastic conectado em", self.port)
                return
            except Exception as e:  # device unplugged or busy: try again later
                log("meshtastic indisponível:", e)
                time.sleep(20)

    def _name(self, node_id):
        n = (self.iface.nodes or {}).get(node_id, {}) if self.iface else {}
        return n.get("user", {}).get("shortName") or n.get("user", {}).get("longName") or str(node_id)

    def _on_text(self, packet, interface):
        msg = {"de": self._name(packet.get("fromId")), "txt": packet.get("decoded", {}).get("text", "")[:160],
               "t": int(packet.get("rxTime") or time.time())}
        with self.lock:
            self.msgs = ([msg] + self.msgs)[: self.max_msgs]

    def status(self):
        if not self.iface:
            return None
        with self.lock:
            return {"nodes": len(self.iface.nodes or {}), "msgs": list(self.msgs)}


# ---------- CYD serial ----------
def find_cyd():
    for dev in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")):
        sysdev = os.path.realpath("/sys/class/tty/%s/device" % os.path.basename(dev))
        for up in (sysdev, os.path.dirname(sysdev), os.path.dirname(os.path.dirname(sysdev))):
            vid, pid = read(up + "/idVendor"), read(up + "/idProduct")
            if vid and "%s:%s" % (vid, pid) == CH340:
                return dev
    return None


def open_cyd(port):
    import serial  # pyserial (apt install python3-serial)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0
    s.dtr = False   # DTR/RTS drive the ESP32 reset and boot pins: keep them released
    s.rts = False
    s.open()
    return s


def build(cfg, mesh):
    d = {"v": 1, "t": int(time.time()), "host": cfg.get("deck", "name")[:24],
         "cpu": cpu_pct(), "ram": ram_pct(), "temp": temp_c(), "up": uptime_s()}
    for key, fn in (("wifi", wifi), ("bat", lambda: battery(cfg)), ("lte", lambda: lte(cfg)), ("gps", lambda: gps(cfg))):
        try:
            v = fn()
        except Exception as e:  # one broken source must not stop the others
            log(key, "falhou:", e)
            v = None
        if v:
            d[key] = v
    m = mesh.status() if mesh else None
    if m:
        d["mesh"] = m
    alerts = []
    b = d.get("bat")
    if b and b.get("pct", 100) <= cfg.getint("battery", "low_pct") and not b.get("chg"):
        alerts.append("bateria baixa: %d%%" % b["pct"])
    if d["temp"] >= 80:
        alerts.append("placa quente: %d°" % d["temp"])
    if alerts:
        d["alert"] = " · ".join(alerts)
    return d


def main():
    ap = argparse.ArgumentParser(description="Status do cyberdeck para a tela CyberCYDeck")
    ap.add_argument("--config", default="/etc/deckd.ini")
    ap.add_argument("--stdout", action="store_true", help="imprime as linhas em vez de mandar pra serial")
    ap.add_argument("--once", action="store_true", help="manda uma vez e sai")
    args = ap.parse_args()

    cfg = configparser.ConfigParser()
    cfg.read_dict(DEFAULTS)
    cfg.read(args.config)
    interval = max(1.0, cfg.getfloat("deck", "interval"))
    mesh_port = cfg.get("mesh", "port").strip()
    mesh = Mesh(mesh_port, cfg.getint("mesh", "max_msgs")) if mesh_port else None

    cpu_pct()  # first sample for the CPU delta
    time.sleep(0.5)
    out = None
    while True:
        if not args.stdout and out is None:
            port = cfg.get("deck", "port")
            port = find_cyd() if port == "auto" else port
            if not port:
                log("CYD não encontrada no USB; tentando de novo em 10 s")
                time.sleep(10)
                continue
            try:
                out = open_cyd(port)
                log("mandando para", port)
            except Exception as e:
                log("não abriu", port, e)
                time.sleep(10)
                continue
        d = build(cfg, mesh)
        line = "deck " + json.dumps(d, ensure_ascii=False, separators=(",", ":"))
        if len(line.encode()) > 1400 and "mesh" in d:  # the CYD keeps at most 1500 bytes per line
            d["mesh"]["msgs"] = d["mesh"]["msgs"][:1]
            line = "deck " + json.dumps(d, ensure_ascii=False, separators=(",", ":"))
        if args.stdout:
            print(line, flush=True)
        else:
            try:
                out.write((line + "\n").encode())
                out.read(4096)  # drop the CYD log so the buffer never fills
            except Exception as e:
                log("serial caiu:", e)
                try:
                    out.close()
                except Exception:
                    pass
                out = None
        if args.once:
            return
        time.sleep(interval)


if __name__ == "__main__":
    main()
