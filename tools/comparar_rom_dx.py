#!/usr/bin/env python3
"""
Compara a ROM original com a ROM do DX guardada no wario.o2r, banco a banco
(16 KB cada), para saber se trocar as fatias .bin do pacote do jogo pelas da ROM
original é seguro.

Uso:
    python3 comparar_rom_dx.py rom_original.gb wario.o2r
    python3 comparar_rom_dx.py rom_original.gb rom_dx.gb      # também aceita a ROM do DX solta

Para cada banco mostra quantos bytes mudaram e em que faixas. Os bancos que só
existem no DX (de 0x80000 em diante) aparecem como "só no DX".
"""
import io
import sys
import zipfile
from pathlib import Path

BANK = 0x4000
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


def ranges(a, b, base):
    """Faixas de bytes diferentes, juntando buracos de até 16 bytes."""
    out, start, last = [], None, None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
            elif i - last > 16:
                out.append((base + start, base + last))
                start = i
            last = i
    if start is not None:
        out.append((base + start, base + last))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    orig = load_rom(sys.argv[1])
    dx = load_rom(sys.argv[2])
    print(f"Original: {len(orig)} bytes    DX: {len(dx)} bytes\n")
    print("Banco  Faixa             Fatia do .o2r           Bytes diferentes")
    total_same = 0
    for bank in range(max(len(orig), len(dx)) // BANK):
        base = bank * BANK
        a = orig[base:base + BANK]
        b = dx[base:base + BANK]
        label = f"{bank:>5}  0x{base:05X}-0x{base + BANK - 1:05X}  {segment_of(base):<22}"
        if not a:
            print(f"{label}  só no DX")
            continue
        diff = sum(1 for x, y in zip(a, b) if x != y)
        if diff == 0:
            total_same += 1
            print(f"{label}  igual")
            continue
        rs = ranges(a, b, base)
        shown = ", ".join(f"0x{s:05X}-0x{e:05X}" for s, e in rs[:6])
        more = f" (+{len(rs) - 6} faixas)" if len(rs) > 6 else ""
        print(f"{label}  {diff:>5}  {shown}{more}")
    print(f"\n{total_same} banco(s) iguais entre as duas ROMs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
