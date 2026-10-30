#!/usr/bin/env python3
"""
gerar_receita_original_bw.py

Gera original_bw_receita.h (na raiz do projeto): a "receita" que o launcher usa
para criar, dentro do wario.o2r, as mesmas texturas P&B do
gerar_mod_original_bw.py, sem precisar do disassembly.

A receita só tem números: para cada textura, o nome (hash do tile do DX e o
contexto _fXX/_gXX) e de onde tirar o tile na ROM ORIGINAL (offset de um .bin,
ou um .rlc + índice do tile, ou a água 1bpp do mapa), mais o registrador
BGP/OBP0 que dá os tons. Nenhum byte do jogo vai para o header.

A escolha de cada textura (votos, variantes por fase/tela) é a do
gerar_mod_original_bw.py: este script importa as funções dele e, no fim,
roda o próprio gerar_mod_original_bw.py e confere que a receita reproduz
exatamente o mesmo .o2r (mesmos nomes e mesmos PNGs).

Uso (na raiz do projeto, com o disassembly em tools/disassembly):
    python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r
    python3 tools/gerar_receita_original_bw.py base_WL-SM3.gb wario.o2r --conferir   # não grava o .h

O segundo arquivo é a ROM do DX (.gb) ou o wario.o2r do jogo. Rode de novo
sempre que mudar o gerar_mod_original_bw.py: a versão da receita muda e o jogo
pede para recriar o wario.o2r pelo launcher.
"""

import argparse
import hashlib
import io
import os
import subprocess
import sys
import tempfile
import zipfile
import zlib
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gerar_mod_original_bw as g  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "original_bw_receita.h"

# ----------------------------------------------------------------------------
# Pares (tile original, tile DX) com a origem do tile original na ROM original
# (mesma lógica de g.asset_pairs / g.agua_1bpp_pairs)
# ----------------------------------------------------------------------------

class Recipe:
    def __init__(self):
        self.assets = []          # (offset, tamanho) dos .rlc na ROM original
        self.asset_index = {}

    def asset(self, off, length):
        key = (off, length)
        if key not in self.asset_index:
            self.asset_index[key] = len(self.assets)
            self.assets.append(key)
        return self.asset_index[key]


def asset_pairs_src(rel, orig_rom, dx_rom, rcp):
    """Igual a g.asset_pairs, mais a origem de cada tile original."""
    raw = (g.GFX_DIR / rel).read_bytes()
    pos = orig_rom.find(raw)
    if rel.endswith(".rlc"):
        orig = g.gfx_decomp_rlc(raw)
        dx = g.gfx_decomp_rlc(dx_rom[pos:pos + 0x3000]) if pos >= 0 else orig
        pairs = list(zip(g.tile_list(orig), g.tile_list(dx)))
        if pos >= 0:
            aid = rcp.asset(pos, len(raw))
            return [(ot, dt, ("rlc", aid, i)) for i, (ot, dt) in enumerate(pairs)]
        return [(ot, dt, None) for ot, dt in pairs]

    if len(raw) % 16 != 0:
        return []
    if pos >= 0 and pos + len(raw) <= len(dx_rom):
        pairs = list(zip(g.tile_list(raw), g.tile_list(dx_rom[pos:pos + len(raw)])))
        return [(ot, dt, ("rom", pos + i * 16)) for i, (ot, dt) in enumerate(pairs)]

    out = []
    for t in g.tile_list(raw):
        p = orig_rom.find(t)
        out.append((t, dx_rom[p:p + 16] if p >= 0 else t, ("rom", p) if p >= 0 else None))
    return out


def agua_pairs_src(orig_rom, dx_rom, rcp):
    """Igual a g.agua_1bpp_pairs, mais a origem."""
    out = []
    rlc_cache = {}
    for rel, frames, rlc_rel, dests in g.AGUA_1BPP:
        data = (g.GFX_DIR / rel).read_bytes()
        stored = bytes(data[j * 8 + i] for i in range(8) for j in range(frames))
        pos = orig_rom.find(stored)
        dx_data = data
        if pos >= 0:
            dx_stored = dx_rom[pos:pos + len(stored)]
            dx_data = bytes(dx_stored[i * frames + j] for j in range(frames) for i in range(8))

        if rlc_rel not in rlc_cache:
            rlc_cache[rlc_rel] = asset_pairs_src(rlc_rel, orig_rom, dx_rom, rcp)
        base = rlc_cache[rlc_rel]
        for addr in dests:
            t = (addr - 0x8000) // 16
            if t < len(base):
                base_orig, base_dx, base_src = base[t]
            else:
                base_orig, base_dx, base_src = bytes(16), bytes(16), ("zero",)
            for j in range(frames):
                lo_o = data[j * 8:(j + 1) * 8]
                lo_d = dx_data[j * 8:(j + 1) * 8]
                to = bytes(b for y in range(8) for b in (lo_o[y], base_orig[y * 2 + 1]))
                td = bytes(b for y in range(8) for b in (lo_d[y], base_dx[y * 2 + 1]))
                src = None
                if pos >= 0 and base_src is not None:
                    if base_src[0] == "rlc":
                        src = ("water", pos, frames, j, base_src[1], base_src[2])
                    elif base_src[0] == "zero":
                        src = ("water", pos, frames, j, -1, 0)
                out.append((to, td, src))
    return out

# ----------------------------------------------------------------------------
# Execução da receita (o mesmo que o C++ faz no launcher)
# ----------------------------------------------------------------------------

def tile_from_source(src, orig_rom, rlc_bufs):
    kind = src[0]
    if kind == "rom":
        return orig_rom[src[1]:src[1] + 16]
    if kind == "rlc":
        buf = rlc_bufs[src[1]]
        return buf[src[2] * 16:src[2] * 16 + 16]
    if kind == "water":
        _, pos, frames, j, aid, t = src
        out = bytearray(16)
        for y in range(8):
            out[y * 2] = orig_rom[pos + y * frames + j]
            out[y * 2 + 1] = rlc_bufs[aid][t * 16 + y * 2 + 1] if aid >= 0 else 0
        return bytes(out)
    raise ValueError(src)


def decompress_assets(rcp, orig_rom):
    return [g.gfx_decomp_rlc(orig_rom[off:off + n]) for off, n in rcp.assets]

# ----------------------------------------------------------------------------

def build(orig_rom, dx_rom):
    """Mesmas escolhas do main() de gerar_mod_original_bw.py.
    Devolve (rcp, textures) com textures = {nome: (tipo, tile_original, reg)}."""
    rcp = Recipe()
    labels = g.read_gfx_labels()
    levels = g.read_level_rooms(labels)

    level_bgp = defaultdict(Counter)
    for _, rooms in levels:
        for rel, bgp in rooms:
            level_bgp[rel][bgp] += 1

    def asset_bgp(rel):
        name = rel.rsplit("/", 1)[-1]
        if name in g.PALETA_TELA:
            return g.PALETA_TELA[name][0]
        if rel in level_bgp:
            return level_bgp[rel].most_common(1)[0][0]
        return g.BGP_PADRAO

    def asset_obp(rel):
        return g.PALETA_TELA.get(rel.rsplit("/", 1)[-1], (None, g.OBP0_PADRAO))[1]

    src_of = {}       # bytes do tile original -> primeira origem conhecida
    pairs_of = {}
    for f in sorted(g.GFX_DIR.rglob("*")):
        rel = f.relative_to(g.GFX_DIR).as_posix()
        if f.suffix not in (".bin", ".rlc") or rel.startswith("_custom/"):
            continue
        if "_anim" in rel and rel.startswith("maps/") and rel not in (
                "maps/ssteacup_anim.bin", "maps/stovecanyon_anim.bin"):
            continue
        pairs_of[rel] = asset_pairs_src(rel, orig_rom, dx_rom, rcp)
    pairs_of["maps/agua_1bpp"] = agua_pairs_src(orig_rom, dx_rom, rcp)
    for pairs in pairs_of.values():
        for ot, _, src in pairs:
            if src is not None and ot not in src_of:
                src_of[ot] = src

    votes = defaultdict(Counter)
    ctx_votes = defaultdict(lambda: defaultdict(Counter))

    def vote(table, rel, i, orig_t, dx_t, bgp):
        for kind in g.asset_kinds(rel, i):
            reg = bgp if kind == "b" else asset_obp(rel)
            table[(kind, g.fnv1a_64(dx_t))][(orig_t, reg)] += 1

    for rel, pairs in pairs_of.items():
        bgp = asset_bgp(rel)
        for i, (ot, dt, _) in enumerate(pairs):
            vote(votes, rel, i, ot, dt, bgp)
        ctx = g.CONTEXTO_TELA.get(rel.rsplit("/", 1)[-1])
        if ctx:
            for i, (ot, dt, _) in enumerate(pairs):
                vote(ctx_votes[ctx], rel, i, ot, dt, bgp)

    for level_id, rooms in levels:
        table = ctx_votes[f"f{level_id:02x}"]
        for rel, bgp in rooms:
            for ot, dt, _ in pairs_of.get(rel, []):
                table[("b", g.fnv1a_64(dt))][(ot, bgp)] += 1

    textures = {}
    default = {}
    for (kind, h), counter in votes.items():
        render = counter.most_common(1)[0][0]
        default[(kind, h)] = render
        textures[f"{kind}_{h:016x}"] = (kind, h, "", render)
    for ctx, table in ctx_votes.items():
        for (kind, h), counter in table.items():
            render = counter.most_common(1)[0][0]
            if default.get((kind, h)) != render:
                textures[f"{kind}_{h:016x}_{ctx}"] = (kind, h, ctx, render)

    # Origem de cada tile usado; sem origem conhecida, procura os 16 bytes na ROM
    missing = 0
    for _, _, _, (tile, _) in textures.values():
        if tile not in src_of:
            p = orig_rom.find(tile)
            if p >= 0:
                src_of[tile] = ("rom", p)
            else:
                missing += 1
    return rcp, textures, src_of, missing


def emit_header(rcp, textures, src_of, orig_sha, dx_sha):
    sources, source_index = [], {}
    rows = []
    for name in sorted(textures):
        kind, h, ctx, (tile, reg) = textures[name]
        src = src_of[tile]
        if src not in source_index:
            source_index[src] = len(sources)
            sources.append(src)
        rows.append((h, source_index[src], reg, kind, ctx))

    def src_row(s):
        if s[0] == "rom":
            return (0, s[1], 0, 0, -1, 0)
        if s[0] == "rlc":
            return (1, 0, 0, 0, s[1], s[2])
        _, pos, frames, j, aid, t = s
        return (2, pos, frames, j, aid, t)

    body = io.StringIO()
    body.write("static const OriginalBwAsset kOriginalBwAssets[] = {\n")
    for off, n in rcp.assets:
        body.write(f"    {{0x{off:05X}u, {n}u}},\n")
    body.write("};\n\n")
    body.write("/* tipo (0 = ROM, 1 = .rlc, 2 = água 1bpp), offset, quadros, quadro, .rlc, tile */\n")
    body.write("static const OriginalBwSource kOriginalBwSources[] = {\n")
    for s in sources:
        t, off, fr, j, aid, tile = src_row(s)
        body.write(f"    {{{t}, {fr}, {j}, {aid}, {tile}, 0x{off:05X}u}},\n")
    body.write("};\n\n")
    body.write("/* hash do tile do DX, origem, registrador BGP/OBP0, tipo, contexto */\n")
    body.write("static const OriginalBwTexture kOriginalBwTextures[] = {\n")
    for h, si, reg, kind, ctx in rows:
        body.write(f"    {{0x{h:016X}ull, {si}, 0x{reg:02X}, '{kind}', \"{ctx}\"}},\n")
    body.write("};\n")
    text = body.getvalue()
    version = (zlib.crc32((orig_sha + dx_sha + text).encode()) & 0x7FFFFFFF) or 1

    head = f"""// Gerado por tools/gerar_receita_original_bw.py — não editar.
// Receita das texturas P&B originais: só offsets na ROM original, hashes dos
// tiles do DX e registradores de paleta (nenhum byte do jogo).
#pragma once
#include <cstdint>

#define ORIGINAL_BW_RECIPE_VERSION {version}u
static const char kOriginalBwOrigSha256[] = "{orig_sha}";
static const char kOriginalBwDxSha256[] = "{dx_sha}";

struct OriginalBwAsset {{ uint32_t offset; uint32_t length; }};
struct OriginalBwSource {{ uint8_t type; uint8_t frames; uint8_t frame; int16_t asset; uint16_t tile; uint32_t offset; }};
struct OriginalBwTexture {{ uint64_t hash; uint32_t source; uint8_t reg; char kind; char ctx[4]; }};

"""
    return head + text, version, len(sources)


def png_bytes_from_recipe(textures, src_of, orig_rom, rcp):
    """PNGs como o gerar_mod_original_bw.py faz, mas com o tile tirado da receita."""
    bufs = decompress_assets(rcp, orig_rom)
    out = {}
    for name, (kind, _, _, (tile, reg)) in textures.items():
        t = tile_from_source(src_of[tile], orig_rom, bufs)
        if t != tile:
            raise SystemExit(f"[ERRO] A receita não reproduz o tile de {name}")
        out[f"{name}.png"] = g.make_png_8x8(g.decode_tile_pixels(t), g.dmg_palette(reg, kind == "o"))
    return out


def main():
    p = argparse.ArgumentParser(description="Gera original_bw_receita.h a partir do disassembly.")
    p.add_argument("orig_rom", nargs="?", default="base_WL-SM3.gb")
    p.add_argument("dx_rom", nargs="?", default="wario.o2r", help="ROM do DX (.gb) ou wario.o2r")
    p.add_argument("--conferir", action="store_true", help="confere tudo sem gravar o .h")
    p.add_argument("--sem-comparar", action="store_true",
                   help="não roda o gerar_mod_original_bw.py para comparar")
    args = p.parse_args()

    orig_rom = g.load_rom(args.orig_rom)
    dx_rom = g.load_rom(args.dx_rom)
    orig_sha = hashlib.sha256(orig_rom).hexdigest()
    dx_sha = hashlib.sha256(dx_rom).hexdigest()
    print(f"ROM original: {len(orig_rom)} bytes  sha256 {orig_sha}")
    print(f"ROM do DX:    {len(dx_rom)} bytes  sha256 {dx_sha}")

    rcp, textures, src_of, missing = build(orig_rom, dx_rom)
    if missing:
        print(f"[ERRO] {missing} tile(s) sem origem na ROM original. Nada foi gravado.")
        return 1
    pngs = png_bytes_from_recipe(textures, src_of, orig_rom, rcp)
    header, version, n_sources = emit_header(rcp, textures, src_of, orig_sha, dx_sha)
    print(f"[ok]   {len(textures)} texturas, {n_sources} origens, {len(rcp.assets)} .rlc; versão {version}")

    if not args.sem_comparar:
        with tempfile.TemporaryDirectory() as tmp:
            ref = Path(tmp) / "ref.o2r"
            cmd = [sys.executable, str(Path(g.__file__)), args.orig_rom, args.dx_rom, "-o", str(ref)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                print("[ERRO] gerar_mod_original_bw.py falhou. Nada foi gravado.")
                return 1
            with zipfile.ZipFile(ref) as z:
                ref_files = {n: z.read(n) for n in z.namelist()}
        if ref_files != pngs:
            only_ref = sorted(set(ref_files) - set(pngs))[:5]
            only_rcp = sorted(set(pngs) - set(ref_files))[:5]
            diff = [n for n in pngs if n in ref_files and ref_files[n] != pngs[n]][:5]
            print(f"[ERRO] A receita não bate com o gerar_mod_original_bw.py "
                  f"(só no mod: {only_ref}, só na receita: {only_rcp}, diferentes: {diff}).")
            return 1
        print(f"[ok]   Igual ao gerar_mod_original_bw.py: {len(pngs)} PNGs, mesmos nomes e mesmos bytes.")

    if args.conferir:
        print(f"Conferência: nada foi gravado. Rode sem --conferir para gravar {HEADER.name}.")
        return 0
    old = HEADER.read_text(encoding="utf-8") if HEADER.exists() else None
    if old == header:
        print(f"[ok]   {HEADER.name} já estava igual.")
    else:
        HEADER.write_text(header, encoding="utf-8", newline="\n")
        print(f"[ok]   Gravado {HEADER} ({len(header) // 1024} KB).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
