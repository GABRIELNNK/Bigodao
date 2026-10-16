#!/usr/bin/env python3
"""
Passo 6c: empacota trilhas .ogg num mod (.o2r) de musica.

Uso:
    python3 tools/musica_empacotar.py <pasta_com_oggs> <saida.o2r>
Exemplo:
    python3 tools/musica_empacotar.py ~/trilhas build/mods/trilhas.o2r

Entram os arquivos cujo nome comeca com o numero da musica em hexa
(04.ogg, 04_rice_beach.ogg, 2B - entrada.ogg ...) e o music.json, se houver.
Dentro do .o2r eles ficam em music/. Os .ogg vao sem compressao (ja sao
comprimidos; assim o jogo le direto do arquivo).
"""
import re
import sys
import zipfile
from pathlib import Path

TRACK = re.compile(r"^[0-9a-fA-F]{2}([._\- ].*)?\.ogg$", re.IGNORECASE)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = Path(sys.argv[1]).expanduser()
    out = Path(sys.argv[2]).expanduser()
    if not src.is_dir():
        sys.exit(f"nao achei a pasta {src}")
    tracks = sorted(p for p in src.iterdir() if p.is_file() and TRACK.match(p.name))
    config = [p for p in src.iterdir() if p.is_file() and p.name.lower() == "music.json"]
    if not tracks:
        sys.exit(f"nenhum XX.ogg em {src} (XX = numero da musica em hexa, ex.: 04.ogg)")
    seen = {}
    for p in tracks:
        song = p.name[:2].upper()
        if song in seen:
            print(f"aviso: musica {song} tem dois arquivos ({seen[song]} e {p.name}); vale o ultimo")
        seen[song] = p.name
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w") as z:
        for p in tracks:
            z.write(p, "music/" + p.name, compress_type=zipfile.ZIP_STORED)
        for p in config:
            z.write(p, "music/music.json", compress_type=zipfile.ZIP_DEFLATED)
    print(f"{out}: {len(tracks)} trilha(s) ({' '.join(sorted(seen))})"
          + (", com music.json" if config else ""))


if __name__ == "__main__":
    main()
