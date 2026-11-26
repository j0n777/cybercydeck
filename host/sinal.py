#!/usr/bin/env python3
"""sinal — manda o estado desta máquina para o n8n (workflow n8n/cybercydeck-feed.workflow.json).

A placa mostra a máquina em "Máquinas": CPU, RAM, temperatura, disco e se está online.
Roda em Linux e macOS. Precisa de: pip install psutil

  CYD_SINAL_URL=https://seu-n8n/webhook/cybercydeck-sinal CYD_TOKEN=seu-token python3 sinal.py
  (sem --uma-vez fica mandando a cada 60 s; use --nome para trocar o nome mostrado)
"""
import argparse, json, os, platform, socket, subprocess, sys, time, urllib.request

import psutil


def temperatura():
    try:
        t = psutil.sensors_temperatures()
    except (AttributeError, OSError):
        return -1                     # macOS: o psutil não lê temperatura
    leituras = [s.current for grupo in t.values() for s in grupo if s.current]
    return round(max(leituras)) if leituras else -1


def discos():
    out = []
    for p in psutil.disk_partitions(all=False):
        if p.fstype in ("squashfs", "tmpfs", "devtmpfs", "overlay") or "/snap/" in p.mountpoint:
            continue
        try:
            u = psutil.disk_usage(p.mountpoint)
        except OSError:
            continue
        out.append({"dispositivo": p.device, "montagem": p.mountpoint, "total_gb": round(u.total / 1e9, 1), "pct": u.percent})
    return out


def servicos_falhos():
    if platform.system() != "Linux":
        return []
    try:
        r = subprocess.run(["systemctl", "--failed", "--plain", "--no-legend"], capture_output=True, text=True, timeout=5)
        return [linha.split()[0] for linha in r.stdout.splitlines() if linha.strip()]
    except (OSError, subprocess.TimeoutExpired):
        return []


def estado(nome):
    return {
        "host": nome,
        "cpu_pct": psutil.cpu_percent(interval=1),
        "ram_pct": psutil.virtual_memory().percent,
        "temp": temperatura(),
        "discos": discos(),
        "uptime_s": int(time.time() - psutil.boot_time()),
        "servicos_falhos": servicos_falhos(),
    }


def enviar(url, token, nome):
    corpo = json.dumps(estado(nome)).encode()
    req = urllib.request.Request(f"{url}?chave=maq_{nome}", data=corpo, method="POST",
                                 headers={"Content-Type": "application/json", "x-cyd-token": token})
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.status


def main():
    ap = argparse.ArgumentParser(description="Manda o estado desta máquina para o feed do CyberCYDeck")
    ap.add_argument("--nome", default=socket.gethostname().split(".")[0].lower())
    ap.add_argument("--uma-vez", action="store_true", help="manda uma vez e sai (bom para cron)")
    ap.add_argument("--mostrar", action="store_true", help="só imprime o JSON, não envia")
    a = ap.parse_args()
    if a.mostrar:
        print(json.dumps(estado(a.nome), indent=2, ensure_ascii=False))
        return
    url, token = os.environ.get("CYD_SINAL_URL"), os.environ.get("CYD_TOKEN")
    if not url or not token:
        sys.exit("defina CYD_SINAL_URL e CYD_TOKEN")
    while True:
        try:
            print("enviado", enviar(url, token, a.nome), flush=True)
        except Exception as e:          # rede fora: tenta de novo na próxima volta
            print("falhou:", e, flush=True)
        if a.uma_vez:
            break
        time.sleep(60)


if __name__ == "__main__":
    main()
