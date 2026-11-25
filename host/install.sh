#!/usr/bin/env bash
# Instala o deckd no Linux do deck (Debian/Ubuntu/Armbian). Rode com sudo, de dentro da pasta host/.
set -euo pipefail
apt-get install -y python3 python3-serial
id deck >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin deck
getent group i2c >/dev/null || groupadd --system i2c
install -d /opt/cybercydeck
install -m 755 deckd.py /opt/cybercydeck/deckd.py
[ -f /etc/deckd.ini ] || install -m 644 deckd.ini.example /etc/deckd.ini
install -m 644 deckd.service /etc/systemd/system/deckd.service
systemctl daemon-reload
systemctl enable --now deckd
echo "Pronto. Logs: journalctl -u deckd -f"
echo "Opcionais: apt install modemmanager gpsd ; pip install --break-system-packages smbus2 meshtastic"
