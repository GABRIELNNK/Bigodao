#!/usr/bin/env python3
"""
Mostra, como imagem, os trechos de 16 bytes (= 1 tile 8x8 de 2 bits) que o patch
DX mudou dentro da parte da ROM que existe nas duas versões (0x00000-0x7FFFF).

Para cada bloco de 16 bytes alinhado que mudou, desenha o "tile" da ROM original
e o da ROM do DX lado a lado, em 4 tons de cinza. Tile de verdade aparece como
desenho; código ou tabela aparece como ruído. Assim dá para ver quais tiles o DX
redesenhou e onde ficam.

Uso:
    python3 tiles_alterados_dx.py rom_original.gb wario.o2r
    python3 tiles_alterados_dx.py rom_original.gb wario.o2r -o pasta_saida

Gera, na pasta de saída (padrão: tiles_dx/):
    tiles_dx.png   grade: cada célula = [original | DX], 8 células por linha
    tiles_dx.txt   lista: linha/coluna da grade, offset na ROM, banco:endereço,
                   fatia do .o2r, bytes diferentes e "faixa" (blocos vizinhos juntos)

Só precisa do Python 3 (sem Pillow).
"""
import argparse
import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path

BANK = 0x4000
COMMON_END = 0x80000          # o que vem depois só existe no DX
SCALE = 4
COLS = 8                      # células por linha da grade
GAP = 4                       # espaço entre original e DX
PAD = 8                       # espaço entre células
SHADES = [(255, 255, 255), (170, 170, 170), (85, 85, 85), (0, 0, 0)]
DIVIDER = (220, 60, 60)
BACKGROUND = (40, 40, 48)
SEGMENTS = [
    ("data/bank0.bin",        0x00000, 0x04000),
    ("gfx/vram_tiles.bin",    0x04000, 0x0C000),
    ("gfx/sprite_tiles.bin",  0x0C000, 0x14000),
    ("audio/wave_tables.bin", 0x14000, 0x18000),
    ("data/logic_banks.bin",  0x18000, None),
]


def load_rom(path):
    data = Path(path).read_bytes()
    if data[:4] != b"PK\x03\x04":
        return data
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        return b"".join(z.read(name) for name, _, _ in SEGMENTS)


def segment_of(offset):
    for name, begin, end in SEGMENTS:
        if offset >= begin and (end is None or offset < end):
            return name
    return "?"


def bank_addr(offset):
    bank = offset // BANK
    addr = offset % BANK + (0 if bank == 0 else BANK)
    return f"{bank:02X}:{addr:04X}"


def decode_tile(block):
    """16 bytes 2bpp -> 64 índices 0..3."""
    px = []
    for y in range(8):
        lo, hi = block[y * 2], block[y * 2 + 1]
        for x in range(8):
            bit = 7 - x
            px.append(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1))
    return px


def write_png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def main():
    p = argparse.ArgumentParser(description="Tiles que o patch DX mudou, lado a lado.")
    p.add_argument("original")
    p.add_argument("dx", help="wario.o2r ou a ROM do DX")
    p.add_argument("-o", "--saida", default="tiles_dx")
    args = p.parse_args()

    orig = load_rom(args.original)
    dx = load_rom(args.dx)
    end = min(len(orig), len(dx), COMMON_END)

    blocks = []
    for off in range(0, end, 16):
        a, b = orig[off:off + 16], dx[off:off + 16]
        if a != b:
            blocks.append((off, sum(1 for x, y in zip(a, b) if x != y)))
    if not blocks:
        print("Nenhuma diferença na parte comum.")
        return 0

    # Faixas: blocos encostados (o mesmo desenho costuma ocupar vários tiles seguidos)
    run_id, runs, prev = [], [], None
    for off, _ in blocks:
        if prev is None or off != prev + 16:
            runs.append([off, off + 15])
        else:
            runs[-1][1] = off + 15
        run_id.append(len(runs) - 1)
        prev = off

    cell_w = 8 * SCALE * 2 + GAP
    cell_h = 8 * SCALE
    rows = (len(blocks) + COLS - 1) // COLS
    W = COLS * cell_w + (COLS + 1) * PAD
    H = rows * cell_h + (rows + 1) * PAD
    img = [[BACKGROUND] * W for _ in range(H)]

    for i, (off, _) in enumerate(blocks):
        r, c = divmod(i, COLS)
        x0 = PAD + c * (cell_w + PAD)
        y0 = PAD + r * (cell_h + PAD)
        for side, rom in enumerate((orig, dx)):
            px = decode_tile(rom[off:off + 16])
            xs = x0 + side * (8 * SCALE + GAP)
            for ty in range(8):
                for tx in range(8):
                    color = SHADES[px[ty * 8 + tx]]
                    for sy in range(SCALE):
                        line = img[y0 + ty * SCALE + sy]
                        for sx in range(SCALE):
                            line[xs + tx * SCALE + sx] = color
        for y in range(y0, y0 + cell_h):          # traço entre original e DX
            for x in range(x0 + 8 * SCALE, x0 + 8 * SCALE + GAP):
                img[y][x] = DIVIDER

    out = Path(args.saida)
    out.mkdir(parents=True, exist_ok=True)
    write_png(out / "tiles_dx.png", W, H, [[ch for px in row for ch in px] for row in img])

    with open(out / "tiles_dx.txt", "w", encoding="utf-8") as f:
        f.write("Cada célula da grade: esquerda = ROM original, direita = DX.\n")
        f.write("linha col  offset   banco:end  fatia                   difs  faixa\n")
        for i, (off, n) in enumerate(blocks):
            r, c = divmod(i, COLS)
            s, e = runs[run_id[i]]
            f.write(f"{r + 1:>5} {c + 1:>3}  0x{off:05X}  {bank_addr(off)}    {segment_of(off):<22} "
                    f"{n:>4}  0x{s:05X}-0x{e:05X} ({(e - s + 1) // 16} bloco(s))\n")
        f.write("\nFaixas:\n")
        for s, e in runs:
            f.write(f"  0x{s:05X}-0x{e:05X}  {bank_addr(s)}  {(e - s + 1) // 16} bloco(s)  {segment_of(s)}\n")

    print(f"{len(blocks)} blocos de 16 bytes diferentes em {len(runs)} faixas (0x00000-0x{end - 1:05X}).")
    print(f"Gravado: {out / 'tiles_dx.png'} ({W}x{H}) e {out / 'tiles_dx.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
