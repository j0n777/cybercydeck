#!/usr/bin/env python3
"""Tira as capturas de tela da placa pela rede (com dados de exemplo) e monta o GIF do README.

  pip install pillow
  python3 tools/capturas.py 192.168.0.50          # salva em docs/telas/

Liga o modo demonstração (/demo?on=1), baixa /shot.bmp de cada tela, salva PNG 2x e um GIF,
e desliga o modo (a placa reinicia com os dados reais).
"""
import io, os, sys, time, urllib.request
from PIL import Image

TELAS = ["home", "btc", "clima", "maquinas", "agenda", "radio", "apps", "tanques", "ssh", "apagao"]

def get(ip, path, timeout=20):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=timeout) as r:
        return r.read()

def main():
    if len(sys.argv) < 2:
        sys.exit("uso: capturas.py IP-da-placa")
    ip, pasta = sys.argv[1], os.path.join(os.path.dirname(__file__), "..", "docs", "telas")
    os.makedirs(pasta, exist_ok=True)
    print(get(ip, "/demo?on=1").decode())
    time.sleep(1.5)
    quadros = []
    try:
        for t in TELAS:
            img = Image.open(io.BytesIO(get(ip, f"/shot.bmp?tela={t}"))).convert("RGB")
            img = img.resize((640, 480), Image.NEAREST)
            img.save(os.path.join(pasta, f"{t}.png"), optimize=True)
            quadros.append(img)
            print("ok", t)
    finally:
        try: get(ip, "/demo?on=0", timeout=3)
        except Exception: pass
    quadros[0].save(os.path.join(pasta, "tour.gif"), save_all=True, append_images=quadros[1:],
                    duration=1800, loop=0, optimize=True)
    print("pronto:", os.path.abspath(pasta))

if __name__ == "__main__":
    main()
