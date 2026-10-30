#!/usr/bin/env python3
"""
gerar_mod_original_bw.py

Gera um pacote de mod de textura (.o2r) para a pasta 'mods/',
contendo os tiles gráficos do jogo (cenários, cavernas, nuvens,
blocos quebráveis, água animada, Wario, inimigos, telas e mapas)
desenhados no preto e branco do Game Boy original (DMG).

Como o motor acha a textura: pelo hash FNV-1a dos 16 bytes que o DX pôs
na VRAM (ppu_native.c, decode_tile). O PNG é desenhado como está, sem
paleta. Então, para cada tile, o nome do arquivo vem do tile do DX e o
desenho vem do tile da ROM original, já com as cores que o jogo original
mostraria (registradores BGP/OBP0 de cada tela, tirados do disassembly).

Os pares original/DX saem do disassembly (tools/disassembly/data/gfx):
  - .bin: a fatia achada na ROM original e a mesma fatia na ROM do DX
    (o DX alterou os gráficos no lugar; os bancos novos dele são tabelas);
  - .rlc: o arquivo original descompactado e o DX descompactado no mesmo
    offset (o DX recoloriu título, mapas, etc. sem mudar de lugar);
  - água do mapa: frames 1bpp intercalados que o jogo grava só no plano
    baixo do tile; o plano alto é o que o .rlc do mapa deixou ali.

Quando o mesmo tile do DX vem de desenhos ou paletas diferentes (ex.: a
moeda de shared_04, que o DX igualou à das outras fases), o PNG padrão é o
mais comum e as fases/telas que diferem ganham variantes _fXX/_g00.

Uso:
    python3 tools/gerar_mod_original_bw.py
    python3 tools/gerar_mod_original_bw.py base_WL-SM3.gb hack1.2_WL-SM3.gb -o mods/original_bw.o2r
"""

import argparse
import io
import os
import re
import struct
import zipfile
import zlib
from collections import Counter, defaultdict
from pathlib import Path

DISASM_DIR = Path(__file__).resolve().parent / "disassembly"
GFX_DIR = DISASM_DIR / "data" / "gfx"

# Tons do DMG por "shade" (0 = branco .. 3 = preto), em RGB
DMG_SHADES = [
    (255, 255, 255),
    (170, 170, 170),
    (85,  85,  85),
    (0,   0,   0),
]

# Paletas padrão do jogo (GameInit, Map_LoadPalette, quase todas as fases)
BGP_PADRAO = 0xE1    # índice 0 -> cinza claro, 1 -> branco, 2 -> cinza escuro, 3 -> preto
OBP0_PADRAO = 0x1C   # índice 1 -> preto, 2 -> cinza claro, 3 -> branco (0 é transparente)

# Telas com paleta própria (BGP, OBP0), pelo nome do asset
PALETA_TELA = {
    "title.rlc":       (0xE4, 0xE4),   # Title_InitVars
    "saveselect.rlc":  (0xD0, 0x1C),   # Mode_Title_SaveSelectInit
    "courseclear.rlc": (0xE4, 0x1C),   # Mode_LevelClear_InitCourseClr
    "heartbonus.rlc":  (0xE4, 0x1C),   # HeartBonus_InitVars
}

# Telas que o motor separa por contexto (hd_context_from_ram: modo de jogo)
CONTEXTO_TELA = {
    "title.rlc": "g00",
    "saveselect.rlc": "g00",
}

# Água 1bpp do mapa: (arquivo, frames, .rlc do mapa, tiles de destino na VRAM a partir de $8000)
AGUA_1BPP = [
    ("maps/overworld_anim00.bin", 8, "maps/overworld.rlc", [0x8AA0]),
] + [
    (f"maps/ricebeach_anim{k}.bin", 6, "maps/mtteapot_ricebeach.rlc", [addr])
    for k, addr in enumerate([0x9500, 0x9510, 0x9600, 0x9610, 0x9620,
                              0x9700, 0x9710, 0x9720, 0x9730, 0x9740])
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

def dmg_palette(reg: int, is_obj: bool):
    """Paleta RGBA de 4 cores a partir de um registrador BGP/OBPx."""
    pal = []
    for i in range(4):
        r, g, b = DMG_SHADES[(reg >> (i * 2)) & 3]
        pal.append((r, g, b, 0 if (is_obj and i == 0) else 255))
    return pal

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

# ----------------------------------------------------------------------------
# Disassembly: rótulos, fases e paletas
# ----------------------------------------------------------------------------

def read_gfx_labels():
    """Rótulo -> caminho do asset (relativo a data/gfx), dos INCBIN inteiros."""
    labels = {}
    pat = re.compile(r'^(\w+):\s*INCBIN\s+"data/gfx/([^"]+)"\s*$', re.M)
    for asm in (DISASM_DIR / "src").glob("*.asm"):
        for m in pat.finditer(asm.read_text(errors="replace")):
            labels[m.group(1)] = m.group(2)
    return labels

def read_level_rooms(labels):
    """Lista de (sLevelId, [(asset, BGP), ...]) com a sala inicial, o checkpoint e as portas.

    sLevelId é a posição em Level_HeaderPtrTable, o mesmo número que o motor usa
    no contexto _fXX."""
    table = (DISASM_DIR / "src" / "bank0C.asm").read_text(errors="replace")
    start = table.index("Level_HeaderPtrTable:")
    end = table.index("Level_HeaderCheckpointPtrTable:")
    names = re.findall(r"dw\s+LevelHeader_(\w+)", table[start:end])

    levels = []
    for level_id, name in enumerate(names):
        lvl_dir = DISASM_DIR / "data" / "lvl" / name.lower()
        rooms = []
        for fname in ("header.asm", "header_checkpoint.asm", "door_headers.asm"):
            f = lvl_dir / fname
            if not f.exists():
                continue
            txt = f.read_text(errors="replace")
            for entry in re.split(r"^\w+:\s*$", txt, flags=re.M):
                pal = re.search(r"db\s+\$([0-9A-Fa-f]{2})\s*;\s*(?:BG )?Palette", entry)
                if not pal:
                    continue
                bgp = int(pal.group(1), 16)
                for label in re.findall(r"\b(GFX_\w+)", entry):
                    if label in labels:
                        rooms.append((labels[label], bgp))
        levels.append((level_id, rooms))
    return levels

# ----------------------------------------------------------------------------
# Pares de tiles (original, DX)
# ----------------------------------------------------------------------------

def tile_list(data: bytes):
    return [data[i:i + 16] for i in range(0, len(data) - 15, 16)]

def asset_pairs(rel: str, orig_rom: bytes, dx_rom: bytes):
    """Lista de (tile_original, tile_dx) de um asset do disassembly."""
    raw = (GFX_DIR / rel).read_bytes()
    pos = orig_rom.find(raw)
    if rel.endswith(".rlc"):
        orig = gfx_decomp_rlc(raw)
        dx = gfx_decomp_rlc(dx_rom[pos:pos + 0x3000]) if pos >= 0 else orig
        return list(zip(tile_list(orig), tile_list(dx)))

    if len(raw) % 16 != 0:
        return []
    if pos >= 0 and pos + len(raw) <= len(dx_rom):
        return list(zip(tile_list(raw), tile_list(dx_rom[pos:pos + len(raw)])))

    # Incluído em pedaços (ex.: ssteacup_anim/stovecanyon_anim): tile a tile
    pairs = []
    for t in tile_list(raw):
        p = orig_rom.find(t)
        pairs.append((t, dx_rom[p:p + 16] if p >= 0 else t))
    return pairs

def agua_1bpp_pairs(orig_rom: bytes, dx_rom: bytes):
    """Tiles da água do mapa como ficam na VRAM: plano baixo = frame 1bpp,
    plano alto = o do tile de destino carregado pelo .rlc do mapa."""
    pairs = []
    rlc_cache = {}
    for rel, frames, rlc_rel, dests in AGUA_1BPP:
        data = (GFX_DIR / rel).read_bytes()        # frame j em j*8 .. j*8+7
        stored = bytes(data[j * 8 + i] for i in range(8) for j in range(frames))
        pos = orig_rom.find(stored)
        dx_data = data
        if pos >= 0:
            dx_stored = dx_rom[pos:pos + len(stored)]
            dx_data = bytes(dx_stored[i * frames + j] for j in range(frames) for i in range(8))

        if rlc_rel not in rlc_cache:
            rlc_cache[rlc_rel] = asset_pairs(rlc_rel, orig_rom, dx_rom)
        base = rlc_cache[rlc_rel]
        for addr in dests:
            t = (addr - 0x8000) // 16
            base_orig, base_dx = base[t] if t < len(base) else (bytes(16), bytes(16))
            for j in range(frames):
                lo_o = data[j * 8:(j + 1) * 8]
                lo_d = dx_data[j * 8:(j + 1) * 8]
                to = bytes(b for y in range(8) for b in (lo_o[y], base_orig[y * 2 + 1]))
                td = bytes(b for y in range(8) for b in (lo_d[y], base_dx[y * 2 + 1]))
                pairs.append((to, td))
    return pairs

def asset_kinds(rel: str, index: int):
    """Em que camadas o tile pode aparecer: 'b' (fundo/janela) e/ou 'o' (sprite)."""
    if rel.startswith("hats/"):
        return "o"
    if rel.endswith(".rlc"):
        # Telas inteiras em $8000-$97FF: sprites só alcançam os 256 primeiros tiles
        return "bo" if index < 256 else "b"
    if (rel.startswith(("actor/", "ending/")) or
            rel in ("level/shared_obj.bin", "maps/submap_obj.bin") or
            rel.startswith("level/statusbar_")):
        return "bo"
    return "b"

# ----------------------------------------------------------------------------

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
    if not GFX_DIR.exists():
        raise FileNotFoundError(f"Disassembly não encontrado: {GFX_DIR}")

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print("[2/4] Lendo fases, paletas e gráficos do disassembly...")
    labels = read_gfx_labels()
    levels = read_level_rooms(labels)

    # Paleta de fundo de cada asset de fase: a mais usada nas salas que o carregam
    level_bgp = defaultdict(Counter)
    for _, rooms in levels:
        for rel, bgp in rooms:
            level_bgp[rel][bgp] += 1

    def asset_bgp(rel):
        name = rel.rsplit("/", 1)[-1]
        if name in PALETA_TELA:
            return PALETA_TELA[name][0]
        if rel in level_bgp:
            return level_bgp[rel].most_common(1)[0][0]
        return BGP_PADRAO

    def asset_obp(rel):
        return PALETA_TELA.get(rel.rsplit("/", 1)[-1], (None, OBP0_PADRAO))[1]

    pairs_of = {}
    for f in sorted(GFX_DIR.rglob("*")):
        rel = f.relative_to(GFX_DIR).as_posix()
        if f.suffix not in (".bin", ".rlc") or rel.startswith("_custom/"):
            continue
        if "_anim" in rel and rel.startswith("maps/") and rel not in (
                "maps/ssteacup_anim.bin", "maps/stovecanyon_anim.bin"):
            continue    # 1bpp: tratados à parte (AGUA_1BPP)
        pairs_of[rel] = asset_pairs(rel, orig_rom, dx_rom)
    pairs_of["maps/agua_1bpp"] = agua_1bpp_pairs(orig_rom, dx_rom)

    # Votos: (tipo, hash do DX) -> desenho -> quantas vezes. Desenho = (tile original, registrador)
    votes = defaultdict(Counter)
    ctx_votes = defaultdict(lambda: defaultdict(Counter))

    def vote(table, rel, i, orig_t, dx_t, bgp):
        for kind in asset_kinds(rel, i):
            reg = bgp if kind == "b" else asset_obp(rel)
            table[(kind, fnv1a_64(dx_t))][(orig_t, reg)] += 1

    for rel, pairs in pairs_of.items():
        bgp = asset_bgp(rel)
        for i, (ot, dt) in enumerate(pairs):
            vote(votes, rel, i, ot, dt, bgp)
        ctx = CONTEXTO_TELA.get(rel.rsplit("/", 1)[-1])
        if ctx:
            for i, (ot, dt) in enumerate(pairs):
                vote(ctx_votes[ctx], rel, i, ot, dt, bgp)

    # Fases: cada sala com a sua BGP (só fundo; sprites usam OBP0 em todas)
    for level_id, rooms in levels:
        table = ctx_votes[f"f{level_id:02x}"]
        for rel, bgp in rooms:
            for ot, dt in pairs_of.get(rel, []):
                table[("b", fnv1a_64(dt))][(ot, bgp)] += 1

    print("[3/4] Escolhendo o desenho original de cada tile do DX...")
    textures = {}   # nome -> png
    png_cache = {}

    def png_of(kind, render):
        key = (kind, render)
        if key not in png_cache:
            tile, reg = render
            png_cache[key] = make_png_8x8(decode_tile_pixels(tile), dmg_palette(reg, kind == "o"))
        return png_cache[key]

    default = {}
    ambiguous = 0
    for (kind, h), counter in votes.items():
        render = counter.most_common(1)[0][0]
        default[(kind, h)] = render
        if len({tile for tile, _ in counter}) > 1:
            ambiguous += 1
        textures[f"{kind}_{h:016x}"] = png_of(kind, render)

    variants = Counter()
    for ctx, table in ctx_votes.items():
        for (kind, h), counter in table.items():
            render = counter.most_common(1)[0][0]
            if default.get((kind, h)) != render:
                textures[f"{kind}_{h:016x}_{ctx}"] = png_of(kind, render)
                variants[ctx[0]] += 1

    n_bg = sum(1 for n in textures if n[0] == "b")
    n_obj = len(textures) - n_bg
    print(f"[4/4] Gravando pacote de mod '{out_path}'...")
    print(f"      - Tiles de fundo (BG): {n_bg}")
    print(f"      - Tiles de sprite (OBJ): {n_obj}")
    print(f"      - Variantes por fase (_fXX): {variants['f']}, por tela (_gXX): {variants['g']}")
    print(f"      - Tiles do DX com mais de um desenho original: {ambiguous}")
    print(f"      - Total de texturas P&B: {len(textures)} (o motor carrega até 16384)")

    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name in sorted(textures):
            z.writestr(f"{name}.png", textures[name])

    print("\n Sucesso! Mod gerado com sucesso em:", out_path)
    print("Para usar no jogo:")
    print("1. Abra o jogo")
    print("2. Menu (Esc) > Mods > clique em 'Recarregar' e confira se 'original_bw.o2r' está ativo")
    print("3. Em 'Graphics & Supersampling', selecione 'Internal Resolution: 2x, 3x ou 4x'")

if __name__ == "__main__":
    main()
