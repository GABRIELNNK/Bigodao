#!/usr/bin/env python3
"""
Gera o launcher_assets.h: icone do launcher (PNG) + fontes Nunito.

O icone vem de platform/icone_launcher.png. As fontes vem do launcher_assets.h
atual (blocos LAUNCHER_FONT_SEMIBOLD e LAUNCHER_FONT_EXTRABOLD) ou, se voce
tiver os .ttf, de --semibold / --extrabold. A arte antiga (LAUNCHER_ART_JPG)
nao entra mais.

Uso, na raiz do projeto:
    python3 tools/gerar_launcher_assets.py
    python3 tools/gerar_launcher_assets.py --icone platform/icone_launcher.png --saida launcher_assets.h
"""
import re
import sys
from pathlib import Path


def arg(name, default):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


def c_array(name, data, comment):
    out = [f"// {comment} ({len(data)} bytes)", f"static const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 32):
        out.append(",".join(str(b) for b in data[i:i + 32]) + ",")
    out.append("};")
    out.append(f"static const unsigned int {name}_LEN = {len(data)}u;")
    return "\n".join(out) + "\n"


def font_from_header(text, name):
    m = re.search(r"static const unsigned char " + name + r"\[\] = \{(.*?)\};", text, re.S)
    if not m:
        return None
    return bytes(int(v) for v in m.group(1).replace("\n", "").split(",") if v.strip())


def main():
    icon = Path(arg("--icone", "platform/icone_launcher.png"))
    out = Path(arg("--saida", "launcher_assets.h"))
    if not icon.exists():
        print(f"Nao achei o icone: {icon}")
        return 1
    old = out.read_text(encoding="utf-8") if out.exists() else ""
    fonts = {}
    for name, opt in (("LAUNCHER_FONT_SEMIBOLD", "--semibold"), ("LAUNCHER_FONT_EXTRABOLD", "--extrabold")):
        ttf = arg(opt, None)
        data = Path(ttf).read_bytes() if ttf else font_from_header(old, name)
        if not data:
            print(f"Nao achei a fonte {name} (passe {opt} arquivo.ttf)")
            return 1
        fonts[name] = data
    png = icon.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        print(f"{icon} nao parece um PNG")
        return 1
    text = ("// Gerado por tools/gerar_launcher_assets.py — não editar.\n"
            "// Recursos embutidos do launcher (ícone PNG + fontes Nunito, SIL OFL 1.1).\n"
            "#pragma once\n\n"
            + c_array("LAUNCHER_ICON_PNG", png, icon.name) + "\n"
            + c_array("LAUNCHER_FONT_SEMIBOLD", fonts["LAUNCHER_FONT_SEMIBOLD"], "Nunito-600.ttf") + "\n"
            + c_array("LAUNCHER_FONT_EXTRABOLD", fonts["LAUNCHER_FONT_EXTRABOLD"], "Nunito-800.ttf"))
    out.write_text(text, encoding="utf-8")
    print(f"{out}: icone {len(png)} bytes + fontes "
          f"{len(fonts['LAUNCHER_FONT_SEMIBOLD'])}/{len(fonts['LAUNCHER_FONT_EXTRABOLD'])} bytes (sem a arte antiga)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
