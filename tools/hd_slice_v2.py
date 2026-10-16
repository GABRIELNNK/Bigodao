#!/usr/bin/env python3
"""
hd_slice.py - fatia uma cena pintada em alta resolucao em tiles HD.

Fluxo:
  1. No jogo, F9 (ou "Capturar cena" na secao Desenvolvedor) grava em
     mods/scenes:  scene_NNN_bg.png, scene_NNN_obj.png      (1x, referencia)
                   scene_NNN_bg_4x.png, scene_NNN_obj_4x.png (base para pintar)
                   scene_NNN.json                            (origem de cada pixel)
  2. Pinte por cima de scene_NNN_bg_4x.png (fundo) e/ou scene_NNN_obj_4x.png
     (sprites, com transparencia). Pode pintar em qualquer escala inteira:
     a largura precisa ser 160*N e a altura 144*N.
  3. Rode:
       python3 tools/hd_slice.py mods/scenes/scene_001.json \\
           --bg pintado_bg.png --obj pintado_obj.png
     Os tiles saem em mods/hd como b_<tile>.png e o_<tile>.png (8N x 8N),
     e mods/hd/pack.json guarda a paleta de referencia de cada tile.

Regras:
  - O mesmo tile pode aparecer varias vezes na cena, mas so existe um PNG por
    tile. Se as ocorrencias foram pintadas diferente, o script avisa
    (conflitos) e escolhe conforme --modo:
        primeira  a primeira ocorrencia (de cima para baixo, esq. para dir.)
        comum     a cor mais frequente em cada subpixel
        media     a media das cores de cada subpixel
    --mapa-conflitos grava uma imagem com os pontos em conflito em vermelho.
  - Partes do tile que nao aparecem na tela sao mantidas do PNG HD que ja
    existir; se nao existir, recebem o desenho original ampliado.
  - Sprites: o canal alfa da pintura define a forma (alfa < 128 = vazio).
    Pode pintar dentro da caixa 8x8 de cada parte do sprite, mesmo onde o
    original era transparente.
  - Fundo: a pintura e opaca (o alfa e ignorado).

Precisa do Pillow:  pip install pillow   (ou: sudo apt install python3-pil)
"""
import argparse
import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("Precisa do Pillow: pip install pillow  (ou sudo apt install python3-pil)")
    sys.exit(1)


def argb_to_rgba(c):
    a = (c >> 24) & 0xFF
    if a == 0:
        return (0, 0, 0, 0)
    return ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, a)


def pixels_of(img):
    getter = getattr(img, "get_flattened_data", None)
    return list(getter() if getter else img.getdata())


def original_tile(tile, n):
    """Tile original (64 indices + paleta) ampliado para 8n x 8n."""
    pal = [argb_to_rgba(c) for c in tile["palette"]]
    idx = tile["index"]
    size = 8 * n
    px = [None] * (size * size)
    for y in range(size):
        for x in range(size):
            px[y * size + x] = pal[int(idx[(y // n) * 8 + (x // n)])]
    return px


def _resolve(samples, mode, is_obj):
    """Escolhe a cor final de um subpixel a partir de todas as ocorrencias."""
    if len(samples) == 1 or mode == "primeira":
        return samples[0]
    if mode == "comum":
        counts = {}
        for c in samples:
            counts[c] = counts.get(c, 0) + 1
        best = max(counts.values())
        for c in samples:            # empate: vale a primeira
            if counts[c] == best:
                return c
    # media
    opaque = [c for c in samples if c[3] >= 128]
    if is_obj and len(opaque) * 2 < len(samples):
        return (0, 0, 0, 0)
    pool = opaque if opaque else samples
    k = len(pool)
    return (sum(c[0] for c in pool) // k, sum(c[1] for c in pool) // k,
            sum(c[2] for c in pool) // k, 255)


def slice_layer(scene, layer, painted, n, mode):
    """Devolve ({tile_id: (pixels, covered, conflitos)}, pontos_em_conflito)."""
    w, h = scene["width"], scene["height"]
    tiles_of = scene[layer + "_tile"]
    uvf_of = scene[layer + "_uvf"]
    src = painted.load()
    size = 8 * n
    is_obj = layer == "obj"
    samples = {}      # tid -> lista de 'size*size' listas de (cor, (x, y) na pintura)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            tid = tiles_of[i]
            if tid < 0:
                continue
            uvf = uvf_of[i]
            u, v = uvf & 7, (uvf >> 3) & 7
            fx, fy = bool(uvf & 0x40), bool(uvf & 0x80)
            slots = samples.get(tid)
            if slots is None:
                slots = [None] * (size * size)
                samples[tid] = slots
            for sy in range(n):
                rs = v * n + sy
                if fy:
                    rs = size - 1 - rs
                for sx in range(n):
                    cs = u * n + sx
                    if fx:
                        cs = size - 1 - cs
                    px, py = x * n + sx, y * n + sy
                    r, g, b, a = src[px, py]
                    if is_obj:
                        color = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
                    else:
                        color = (r, g, b, 255)
                    k = rs * size + cs
                    if slots[k] is None:
                        slots[k] = []
                    slots[k].append((color, (px, py)))

    out = {}
    hot = []
    for tid, slots in samples.items():
        pixels = [None] * (size * size)
        covered = [False] * (size * size)
        conflicts = 0
        for k, lst in enumerate(slots):
            if not lst:
                continue
            colors = [c for c, _ in lst]
            final = _resolve(colors, mode, is_obj)
            pixels[k] = final
            covered[k] = True
            for c, pos in lst:
                if c != final:
                    conflicts += 1
                    hot.append(pos)
        out[tid] = (pixels, covered, conflicts)
    return out, hot


def save_conflict_map(painted, hot, path):
    """Pintura esmaecida com os pontos em conflito em vermelho."""
    img = painted.convert("RGBA").copy()
    px = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            gray = (r + g + b) // 6 + 40
            px[x, y] = (gray, gray, gray, 255)
    for x, y in hot:
        px[x, y] = (255, 0, 0, 255)
    img.save(path)


def load_painted(path, n_expected=None):
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    if w % 160 or h % 144 or w // 160 != h // 144:
        raise SystemExit(f"{path}: tamanho {w}x{h} nao e 160N x 144N")
    n = w // 160
    if n_expected and n != n_expected:
        raise SystemExit(f"{path}: escala {n}x diferente da outra camada ({n_expected}x)")
    return img, n


def main():
    ap = argparse.ArgumentParser(description="Fatia uma cena pintada em tiles HD.")
    ap.add_argument("scene", help="scene_NNN.json gravado pelo jogo")
    ap.add_argument("--bg", help="pintura do fundo (160N x 144N)")
    ap.add_argument("--obj", help="pintura dos sprites (160N x 144N, com alfa)")
    ap.add_argument("--out", help="pasta dos tiles HD (padrao: mods/hd ao lado de mods/scenes)")
    ap.add_argument("--substituir", action="store_true",
                    help="nao aproveita o PNG HD que ja existe (recria o tile inteiro)")
    ap.add_argument("--modo", choices=["primeira", "comum", "media"], default="primeira",
                    help="tile repetido pintado diferente: primeira ocorrencia (padrao), "
                         "cor mais comum entre as ocorrencias, ou media das cores")
    ap.add_argument("--mapa-conflitos", action="store_true",
                    help="grava conflitos_bg.png / conflitos_obj.png mostrando em vermelho "
                         "onde a pintura difere do que ficou no tile")
    args = ap.parse_args()
    if not args.bg and not args.obj:
        ap.error("informe --bg e/ou --obj")

    scene_path = Path(args.scene)
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    tiles = scene["tiles"]
    out_dir = Path(args.out) if args.out else scene_path.parent.parent / "hd"
    out_dir.mkdir(parents=True, exist_ok=True)

    n = None
    layers = []
    if args.bg:
        img, n = load_painted(args.bg)
        layers.append(("bg", img))
    if args.obj:
        img, n = load_painted(args.obj, n)
        layers.append(("obj", img))
    size = 8 * n

    pack_path = out_dir / "pack.json"
    pack = {"version": 1, "tiles": {}}
    if pack_path.exists():
        try:
            pack = json.loads(pack_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"aviso: {pack_path} invalido, sera recriado")

    written = 0
    conflicts = []
    for layer, img in layers:
        result, hot = slice_layer(scene, layer, img, n, args.modo)
        if args.mapa_conflitos:
            map_path = out_dir / f"conflitos_{layer}.png"
            save_conflict_map(img, hot, map_path)
            print(f"mapa de conflitos: {map_path}")
        for tid, (pixels, covered, nconf) in result.items():
            tile = tiles[tid]
            name = tile["name"]
            path = out_dir / (name + ".png")
            base = None
            if path.exists() and not args.substituir:
                old = Image.open(path).convert("RGBA")
                if old.size == (size, size):
                    base = pixels_of(old)
                else:
                    print(f"aviso: {path.name} tem {old.size[0]}x{old.size[1]}, "
                          f"recriado em {size}x{size}")
            if base is None:
                base = original_tile(tile, n)
            final = [pixels[k] if covered[k] else base[k] for k in range(size * size)]
            out = Image.new("RGBA", (size, size))
            out.putdata(final)
            out.save(path)
            written += 1
            pack["tiles"][name] = {"palette": tile["palette"], "scale": n}
            if nconf:
                conflicts.append((nconf, name))

    pack_path.write_text(json.dumps(pack, indent=1, sort_keys=True), encoding="utf-8")
    print(f"{written} tiles gravados em {out_dir} (escala {n}x, {size}x{size} px)")
    if conflicts:
        conflicts.sort(reverse=True)
        regra = {"primeira": "vale a primeira ocorrencia",
                 "comum": "vale a cor mais comum",
                 "media": "vale a media"}[args.modo]
        print(f"{len(conflicts)} tiles pintados diferente em lugares diferentes "
              f"({regra}). Os maiores:")
        for nconf, name in conflicts[:10]:
            print(f"  {name}: {nconf} subpixels diferentes")


if __name__ == "__main__":
    main()
