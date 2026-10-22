#!/usr/bin/env python3
"""
hd_empacotar.py - junta os tiles HD de uma pasta num mod .o2r (um ZIP comum
com outra extensao), para colocar em <pasta do jogo>/mods.

    python3 tools/hd_empacotar.py mods/hd mods/meu_mod.o2r

Entram os b_<tile>.png / o_<tile>.png da pasta (e subpastas), inclusive os
por fase e paleta (b_<tile>_f10_p3.png, passo 11a), e os pack.json.
Os mapas de conflito e outros arquivos ficam de fora.

No jogo (Menu > Mods) cada .o2r aparece com uma caixa para ligar/desligar.
Ordem: os .o2r em ordem alfabetica e depois a pasta mods/hd; quando o mesmo
tile esta em mais de um, vale o de baixo. Para mudar a ordem, renomeie
(01_base.o2r, 02_detalhes.o2r...).

Depois de empacotar, apague ou renomeie a pasta mods/hd se nao quiser que
ela continue sobrepondo o .o2r.
"""
import argparse
import re
import sys
import zipfile
from pathlib import Path

# Passo 11a/11b: sufixos opcionais _fXX / _mXX / _gXX (fase / tela do mapa /
# outra tela pelo modo do jogo) e _pN (paleta).
TILE = re.compile(r"^[bo]_[0-9a-fA-F]{16}(_[fmgFMG][0-9a-fA-F]{2})?(_[pP][0-7])?\.png$")


def tile_vazio(path):
    """Passo 7c: PNG sem nenhum ponto visivel (alfa 0 em tudo)."""
    try:
        from PIL import Image
    except ImportError:
        return False   # sem Pillow nao confere
    with Image.open(path) as img:
        if "A" not in img.convert("RGBA").getbands():
            return False
        return img.convert("RGBA").getchannel("A").getextrema()[1] == 0


def main():
    ap = argparse.ArgumentParser(description="Empacota tiles HD num mod .o2r")
    ap.add_argument("pasta", help="pasta com os tiles (ex.: mods/hd)")
    ap.add_argument("saida", help="arquivo .o2r (ex.: mods/meu_mod.o2r)")
    ap.add_argument("--manter-vazios", action="store_true",
                    help="inclui o_*.png 100%% transparentes (escondem o pedaco do sprite)")
    args = ap.parse_args()

    src = Path(args.pasta)
    out = Path(args.saida)
    if not src.is_dir():
        sys.exit(f"{src} nao e uma pasta")
    if out.suffix.lower() != ".o2r":
        sys.exit("a saida precisa terminar em .o2r")

    tiles, packs, vazios = [], [], []
    for p in sorted(src.rglob("*")):
        if not p.is_file():
            continue
        if TILE.match(p.name):
            if p.name.lower().startswith("o_") and not args.manter_vazios and tile_vazio(p):
                vazios.append(p)   # passo 7c
                continue
            tiles.append(p)
        elif p.name.lower() == "pack.json":
            packs.append(p)
    if not tiles:
        if vazios:
            sys.exit(f"todos os {len(vazios)} tiles de {src} sao sprites 100% transparentes "
                     f"(use --manter-vazios se for de proposito)")
        sys.exit(f"nenhum b_<tile>.png / o_<tile>.png em {src}")

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    with zipfile.ZipFile(tmp, "w") as z:
        for p in packs:
            z.write(p, p.relative_to(src).as_posix(), compress_type=zipfile.ZIP_DEFLATED)
        for p in tiles:   # PNG ja e comprimido: guarda sem recomprimir
            z.write(p, p.relative_to(src).as_posix(), compress_type=zipfile.ZIP_STORED)
    tmp.replace(out)
    print(f"{out}: {len(tiles)} tiles, {len(packs)} pack.json")
    if vazios:
        print(f"{len(vazios)} sprite(s) 100% transparente(s) ficaram de fora "
              f"(use --manter-vazios para incluir):")
        for p in vazios[:10]:
            print(f"  {p.relative_to(src).as_posix()}")


if __name__ == "__main__":
    main()
