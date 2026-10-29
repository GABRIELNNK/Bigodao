#!/usr/bin/env python3
"""
gerar_mod_original_bw.py

Gera um pacote de mod de textura (.o2r) para a pasta 'mods/',
contendo TODOS os tiles gráficos do jogo (cenários, cavernas, nuvens,
blocos quebráveis, água animada, Wario e todos os inimigos) desenhados
no preto e branco clássico do Game Boy original (4 tons de cinza / DMG).

Uso:
    python3 tools/gerar_mod_original_bw.py
"""

import argparse
import glob
import io
import os
import struct
import sys
import zipfile
import zlib
from pathlib import Path

# Tons clássicos do Game Boy DMG em escala de cinza (RGBA)
DMG_PALETTE_BG = [
    (255, 255, 255, 255),  # 0: Branco
    (170, 170, 170, 255),  # 1: Cinza claro
    (85,  85,  85,  255),  # 2: Cinza escuro
    (0,   0,   0,   255),  # 3: Preto
]

DMG_PALETTE_OBJ = [
    (0,   0,   0,   0),    # 0: Transparente para sprites
    (170, 170, 170, 255),  # 1: Cinza claro
    (85,  85,  85,  255),  # 2: Cinza escuro
    (0,   0,   0,   255),  # 3: Preto
]

def fnv1a_64(data: bytes) -> int:
    """Calcula o hash FNV-1a de 64 bits (o mesmo usado pelo motor em ppu_native.c)."""
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def decode_tile_pixels(tile_16bytes: bytes):
    """Decodifica 16 bytes 2bpp em uma lista de 64 índices (0..3)."""
    px = [0] * 64
    for y in range(8):
        lo = tile_16bytes[y * 2]
        hi = tile_16bytes[y * 2 + 1]
        for x in range(8):
            bit = 7 - x
            px[y * 8 + x] = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
    return px

def make_png_8x8(indices, palette) -> bytes:
    """Gera o arquivo binário PNG 8x8 RGBA a partir dos índices e da paleta sem precisar de Pillow."""
    raw = bytearray()
    for y in range(8):
        raw.append(0)  # Filtro 0 (None)
        for x in range(8):
            color = palette[indices[y * 8 + x]]
            raw.extend(color)

    def chunk(tag: bytes, data: bytes) -> bytes:
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    return png

def gfx_decomp_rlc(data: bytes, buf_size: int = 0x1800) -> bytes:
    """Descompactador RLC usado nos gráficos comprimidos do Wario Land."""
    out = bytearray(buf_size)
    i = 0
    p = 0
    max_len = len(data)
    while i < max_len:
        b = data[i]
        i += 1
        if b == 0:
            return bytes(out)
        if b & 0x80:
            count = b & 0x7F
            for _ in range(count):
                if i >= max_len:
                    break
                if p >= buf_size:
                    if p == buf_size + 1:
                        return bytes(out)
                    p = 1
                out[p] = data[i]
                i += 1
                p += 2
        else:
            count = b
            source = data[i]
            i += 1
            for _ in range(count):
                if p >= buf_size:
                    if p == buf_size + 1:
                        return bytes(out)
                    p = 1
                out[p] = source
                p += 2
    return bytes(out)

def load_rom(path: str) -> bytes:
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"Arquivo não encontrado: {path}")
    data = p.read_bytes()
    if data[:4] == b"PK\x03\x04":
        segments = [
            "data/bank0.bin",
            "gfx/vram_tiles.bin",
            "gfx/sprite_tiles.bin",
            "audio/wave_tables.bin",
            "data/logic_banks.bin"
        ]
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            return b"".join(z.read(name) for name in segments if name in z.namelist())
    return data

def main():
    parser = argparse.ArgumentParser(description="Gera o pacote de mod original_bw.o2r em preto e branco completo.")
    parser.add_argument("orig_rom", nargs="?", default="base_WL-SM3.gb", help="Caminho da ROM original (.gb)")
    parser.add_argument("dx_rom", nargs="?", default="hack1.2_WL-SM3.gb", help="Caminho da ROM DX (.gb ou wario.o2r)")
    parser.add_argument("-o", "--output", default="mods/original_bw.o2r", help="Arquivo .o2r de saída")
    args = parser.parse_args()

    dx_path = args.dx_rom
    if not os.path.exists(dx_path) and os.path.exists("wario.o2r"):
        dx_path = "wario.o2r"

    print(f"[1/4] Carregando ROM original ({args.orig_rom}) e ROM DX ({dx_path})...")
    orig_rom = load_rom(args.orig_rom)
    dx_rom = load_rom(dx_path)

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    tiles_bg = {}   # hash -> png_bytes
    tiles_obj = {}  # hash -> png_bytes

    def add_tile(orig_t: bytes, dx_t: bytes, is_bg: bool, is_obj: bool):
        if len(orig_t) != 16 or len(dx_t) != 16:
            return
        h_dx = fnv1a_64(dx_t)
        h_orig = fnv1a_64(orig_t)
        hashes = {h_dx, h_orig}

        indices = decode_tile_pixels(orig_t)
        if is_bg:
            png_bg = None
            for h in hashes:
                if h not in tiles_bg:
                    if png_bg is None:
                        png_bg = make_png_8x8(indices, DMG_PALETTE_BG)
                    tiles_bg[h] = png_bg

        if is_obj:
            png_obj = None
            for h in hashes:
                if h not in tiles_obj:
                    if png_obj is None:
                        png_obj = make_png_8x8(indices, DMG_PALETTE_OBJ)
                    tiles_obj[h] = png_obj

    print("[2/4] Extraindo todos os gráficos dos arquivos de asset (.bin e .rlc)...")
    gfx_dir = Path("tools/disassembly/data/gfx")
    if gfx_dir.exists():
        # Processa todos os 194 arquivos .bin
        for b_path in gfx_dir.rglob("*.bin"):
            orig_data = b_path.read_bytes()
            if len(orig_data) % 16 != 0:
                continue

            path_str = str(b_path).replace("\\", "/")
            is_actor = "actor" in path_str or "hats" in path_str or "shared_obj" in path_str
            is_level = "level" in path_str or "map" in path_str or "bg" in path_str or "ending" in path_str

            # O Wario, inimigos e blocos quebráveis (e partículas) podem aparecer como sprite ou fundo
            is_obj = is_actor or ("shared" in path_str)
            is_bg = is_level or ("shared" in path_str)

            # Localiza a fatia na ROM original e na ROM DX
            pos = orig_rom.find(orig_data)
            if pos != -1 and pos + len(orig_data) <= len(dx_rom):
                dx_data = dx_rom[pos : pos + len(orig_data)]
            else:
                dx_data = orig_data

            num_tiles = len(orig_data) // 16
            for i in range(num_tiles):
                ot = orig_data[i * 16 : (i + 1) * 16]
                dt = dx_data[i * 16 : (i + 1) * 16]
                add_tile(ot, dt, is_bg, is_obj)

        # Processa todas as telas comprimidas RLC (Título, Bônus, Mapas, etc.)
        for rlc_file in gfx_dir.rglob("*.rlc"):
            try:
                dec = gfx_decomp_rlc(rlc_file.read_bytes())
                num_tiles = len(dec) // 16
                for i in range(num_tiles):
                    t = dec[i * 16 : (i + 1) * 16]
                    add_tile(t, t, is_bg=True, is_obj=True)
            except Exception:
                pass

    print("[3/4] Mapeando bancos clássicos de VRAM (0x04000 a 0x14000)...")
    for off in range(0x04000, 0x14000, 16):
        orig_t = orig_rom[off : off + 16]
        dx_t = dx_rom[off : off + 16]
        if (all(b == 0 for b in orig_t) or all(b == 0xFF for b in orig_t)) and \
           (all(b == 0 for b in dx_t) or all(b == 0xFF for b in dx_t)):
            continue
        add_tile(orig_t, dx_t, is_bg=(off < 0x0C000), is_obj=(off >= 0x0C000))

    print(f"[4/4] Gravando pacote de mod '{out_path}'...")
    print(f"      - Tiles de fundo (BG): {len(tiles_bg)}")
    print(f"      - Tiles de sprite (OBJ): {len(tiles_obj)}")
    print(f"      - Total de texturas P&B: {len(tiles_bg) + len(tiles_obj)}")

    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for h, png_data in tiles_bg.items():
            z.writestr(f"b_{h:016x}.png", png_data)
        for h, png_data in tiles_obj.items():
            z.writestr(f"o_{h:016x}.png", png_data)

    print("\n Sucesso! Mod gerado com sucesso em:", out_path)
    print("Para usar no jogo:")
    print("1. Abra o jogo")
    print("2. Menu (Esc) > Mods > clique em 'Recarregar' e confira se 'original_bw.o2r' está ativo")
    print("3. Em 'Graphics & Supersampling', selecione 'Internal Resolution: 2x, 3x ou 4x'")

if __name__ == "__main__":
    main()
