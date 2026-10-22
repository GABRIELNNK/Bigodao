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
     o tamanho precisa ser o da cena vezes N (160x144, ou 256x144 quando a
     captura foi feita com o widescreen ligado, laterais incluidas).
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
  - (passo 7b) O que foi pintado vence o que ficou igual ao original: basta
    pintar uma ocorrencia de cada tile (ex.: so o Wario olhando para um lado).
    --todas-votam volta ao comportamento antigo.
  - (passo 7c) Pintura vazia nao estraga nada: um tile sem nenhum ponto
    pintado (100% transparente em todas as ocorrencias) nao e gravado, e no
    fundo os pontos transparentes ficam como estavam. Para esconder de
    proposito um pedaco de sprite com um tile transparente: --manter-vazios.
  - Partes do tile que nao aparecem na tela sao mantidas do PNG HD que ja
    existir; se nao existir, recebem o desenho original ampliado.
  - Sprites: o canal alfa da pintura define a forma (alfa < 128 = vazio).
    Pode pintar dentro da caixa 8x8 de cada parte do sprite, mesmo onde o
    original era transparente.
  - Fundo: a pintura e opaca (o alfa e ignorado).
  - Se o mesmo tile aparece com paletas diferentes na cena, as cores sao
    convertidas para a paleta de referencia (usa scene_NNN_bg.png e
    scene_NNN_obj.png, ao lado do .json). O jogo recolore ao desenhar.

Passo 11a - tiles por fase e por paleta:
  O jogo reaproveita o mesmo desenho de tile em varias fases e com paletas
  diferentes. Por padrao o FUNDO sai so para a fase e a paleta da captura e os
  SPRITES saem genericos (valem em todo lugar):
      b_<tile>_f10_p3.png   fundo, fase $10, paleta 3
      o_<tile>.png          sprite, todo lugar
  (_m44 no lugar de _f10 = tela do mapa da pagina DX $44; _g00 = outra tela
  pelo modo do jogo, ex.: titulo/slots, passo 11b). No jogo vale o mais
  especifico: fase+paleta, paleta, fase, generico. Lista das fases:
  tools/fases.csv.
      --fundo / --sprites  fase+paleta | paleta | fase | generico
      --generico           os dois genericos (como antes do 11a)
      --contexto f10       forca o contexto (cenas gravadas antes do 11a nao
                           tem contexto nem numero de paleta)

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
    if samples and isinstance(samples[0], tuple) and len(samples[0]) == 2 and isinstance(samples[0][1], bool):
        # Passo 7b: (cor, pintado). O que foi pintado vence o que ficou igual
        # ao original; se nada foi pintado, todas valem.
        painted = [c for c, p in samples if p]
        samples = painted if painted else [c for c, _ in samples]
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


def _normalize(color, orig, ref):
    """Leva a cor pintada para a paleta de referencia do tile.

    Se este ponto da cena foi desenhado com outra paleta (o mesmo tile aparece
    com cores diferentes), a cor pintada e escalada por ref/original, canal a
    canal. O jogo faz a conta inversa ao desenhar com a paleta da vez."""
    if color[3] == 0 or orig[:3] == ref[:3]:
        return color
    out = []
    for c, o, r in zip(color[:3], orig[:3], ref[:3]):
        if o:
            v = (c * r + o // 2) // o
        else:
            v = r if r else c
        out.append(max(0, min(255, v)))
    return (out[0], out[1], out[2], color[3])


def slice_layer(scene, layer, painted, n, mode, reference=None, prefer_painted=True,
                keep_empty=False, group_of=None):
    """Devolve ({nome: (pixels, covered, conflitos, tid_referencia)},
    pontos_em_conflito, nomes_sem_pintura).

    group_of[tid] = nome do PNG de saida (passo 11a). Entradas da cena com o
    mesmo nome (o mesmo tile com outra paleta, num tile generico) viram um PNG
    so; a paleta de referencia e a da primeira entrada do grupo."""
    w, h = scene["width"], scene["height"]
    tiles_of = scene[layer + "_tile"]
    uvf_of = scene[layer + "_uvf"]
    tiles = scene["tiles"]
    if group_of is None:
        group_of = [t["name"] for t in tiles]
    ref_tid = {}
    for tid, name in enumerate(group_of):
        ref_tid.setdefault(name, tid)
    ref_px = reference.load() if reference is not None else None
    src = painted.load()
    size = 8 * n
    is_obj = layer == "obj"
    samples = {}      # tid -> lista de 'size*size' listas de (cor, (x, y) na pintura)
    # Passo 7c: uma ocorrencia (caixa 8x8 na tela) sem nenhum ponto pintado
    # (alfa < 128 em tudo) nao conta: o tile fica como estava.
    alpha = painted.getchannel("A").load()
    box_paint = {}
    seen_tiles = set()
    for y in range(h):
        for x in range(w):
            tid = tiles_of[y * w + x]
            if tid < 0:
                continue
            seen_tiles.add(group_of[tid])
            uvf = uvf_of[y * w + x]
            key = (x - (uvf & 7), y - ((uvf >> 3) & 7), tid)
            if box_paint.get(key):
                continue
            hit = False
            for sy in range(n):
                for sx in range(n):
                    if alpha[x * n + sx, y * n + sy] >= 128:
                        hit = True
                        break
                if hit:
                    break
            box_paint[key] = hit
    for y in range(h):
        for x in range(w):
            i = y * w + x
            tid = tiles_of[i]
            if tid < 0:
                continue
            uvf = uvf_of[i]
            u, v = uvf & 7, (uvf >> 3) & 7
            fx, fy = bool(uvf & 0x40), bool(uvf & 0x80)
            if not keep_empty and not box_paint.get((x - u, y - v, tid)):
                continue          # passo 7c: caixa vazia na pintura
            gname = group_of[tid]
            slots = samples.get(gname)
            if slots is None:
                slots = [None] * (size * size)
                samples[gname] = slots
            orig = ref_color = None
            if ref_px is not None:
                tile = tiles[ref_tid[gname]]
                tu = 7 - u if fx else u
                tv = 7 - v if fy else v
                idx = int(tile["index"][tv * 8 + tu])
                orig = ref_px[x, y]
                ref_color = argb_to_rgba(tile["palette"][idx])
                if is_obj and idx == 0:
                    orig = None          # ponto transparente no original
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
                    if not is_obj and a < 128:
                        continue      # passo 7c: fundo transparente = fica como estava
                    if is_obj:
                        color = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
                    else:
                        color = (r, g, b, 255)
                    # Passo 7b: igual ao original neste ponto = nao pintado.
                    is_painted = True
                    if ref_px is not None and prefer_painted:
                        o = ref_px[x, y]
                        if is_obj and orig is None:
                            is_painted = a >= 128
                        else:
                            is_painted = not ((a >= 128 or not is_obj) and (r, g, b) == tuple(o[:3]))
                    if orig is not None:
                        color = _normalize(color, orig, ref_color)
                    k = rs * size + cs
                    if slots[k] is None:
                        slots[k] = []
                    slots[k].append((color, (px, py), is_painted))

    out = {}
    hot = []
    for gname, slots in samples.items():
        pixels = [None] * (size * size)
        covered = [False] * (size * size)
        conflicts = 0
        for k, lst in enumerate(slots):
            if not lst:
                continue
            votes = [(c, p) for c, _, p in lst]
            final = _resolve(votes, mode, is_obj)
            pixels[k] = final
            covered[k] = True
            any_painted = any(p for _, _, p in lst)
            for c, pos, p in lst:
                if any_painted and not p:
                    continue          # nao pintado: nao conta como conflito
                if c != final:
                    conflicts += 1
                    hot.append(pos)
        if any(covered):
            out[gname] = (pixels, covered, conflicts, ref_tid[gname])
    skipped = sorted(seen_tiles - set(out))
    return out, hot, skipped


POLITICAS = ["fase+paleta", "paleta", "fase", "generico"]


def output_names(scene, layer, policy, ctx_override=None):
    """Passo 11a: nome do PNG de cada entrada da cena, conforme a politica.

    Devolve (nomes, avisos). Sem numero de paleta (cena antiga) o sufixo _pN
    fica de fora; sem contexto (cena antiga, titulo, telas especiais) o _fXX
    fica de fora."""
    kind = "b" if layer == "bg" else "o"
    names, falta_pal, falta_ctx = [], 0, 0
    for t in scene["tiles"]:
        name = t["name"]
        if t.get("kind", name[0]) != kind:
            names.append(name)
            continue
        if policy in ("fase+paleta", "fase"):
            ctx = ctx_override if ctx_override is not None else t.get("context", scene.get("context", ""))
            ctx = (ctx or "").lower()
            if ctx:
                name += "_" + ctx
            else:
                falta_ctx += 1
        if policy in ("fase+paleta", "paleta"):
            if "pal" in t:
                name += "_p%d" % (int(t["pal"]) & 7)
            else:
                falta_pal += 1
        names.append(name)
    avisos = []
    camada = "fundo" if layer == "bg" else "sprites"
    if falta_ctx:
        avisos.append(f"{camada}: cena sem contexto (fase/mapa); os tiles saem sem _fXX/_mXX "
                      f"(use --contexto fXX se for uma fase)")
    if falta_pal:
        avisos.append(f"{camada}: cena antiga, sem numero de paleta; os tiles saem sem _pN")
    return names, avisos


def generic_fallbacks(name):
    """Do mais especifico para o generico, como o jogo procura (sem o proprio)."""
    parts = name.split("_")
    base = "_".join(parts[:2])
    ctx = next((p for p in parts[2:] if p[:1] in "fmg"), None)
    pal = next((p for p in parts[2:] if p[:1] == "p"), None)
    out = []
    if ctx and pal:
        out += [f"{base}_{pal}", f"{base}_{ctx}"]
    if ctx or pal:
        out.append(base)
    return out


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


def load_painted(path, base_w, base_h, n_expected=None):
    """A pintura tem o tamanho da cena (160x144, ou 256x144 com widescreen) vezes N."""
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    if w % base_w or h % base_h or w // base_w != h // base_h:
        raise SystemExit(f"{path}: tamanho {w}x{h} nao e {base_w}N x {base_h}N "
                         f"(a cena tem {base_w}x{base_h})")
    n = w // base_w
    if n_expected and n != n_expected:
        raise SystemExit(f"{path}: escala {n}x diferente da outra camada ({n_expected}x)")
    return img, n


def main():
    ap = argparse.ArgumentParser(description="Fatia uma cena pintada em tiles HD.")
    ap.add_argument("scene", help="scene_NNN.json gravado pelo jogo")
    ap.add_argument("--bg", help="pintura do fundo (tamanho da cena x N)")
    ap.add_argument("--obj", help="pintura dos sprites (tamanho da cena x N, com alfa)")
    ap.add_argument("--out", help="pasta dos tiles HD (padrao: mods/hd ao lado de mods/scenes)")
    ap.add_argument("--substituir", action="store_true",
                    help="nao aproveita o PNG HD que ja existe (recria o tile inteiro)")
    ap.add_argument("--modo", choices=["primeira", "comum", "media"], default="primeira",
                    help="tile repetido pintado diferente: primeira ocorrencia (padrao), "
                         "cor mais comum entre as ocorrencias, ou media das cores")
    ap.add_argument("--mapa-conflitos", action="store_true",
                    help="grava conflitos_bg.png / conflitos_obj.png mostrando em vermelho "
                         "onde a pintura difere do que ficou no tile")
    ap.add_argument("--todas-votam", action="store_true",
                    help="(passo 7b) ocorrencias deixadas como o original tambem contam; "
                         "por padrao, o que foi pintado vence o que ficou igual ao original")
    ap.add_argument("--manter-vazios", action="store_true",
                    help="(passo 7c) sprite totalmente transparente na pintura vira tile "
                         "HD transparente (esconde o pedaco no jogo); por padrao, tile sem "
                         "nenhuma pintura nao e gravado e fica como estava")
    ap.add_argument("--fundo", choices=POLITICAS, default="fase+paleta",
                    help="(passo 11a) para onde vale o fundo pintado: so nesta fase e nesta "
                         "paleta (padrao), so nesta paleta, so nesta fase, ou em todo lugar")
    ap.add_argument("--sprites", choices=POLITICAS, default="generico",
                    help="(passo 11a) idem para os sprites (padrao: em todo lugar)")
    ap.add_argument("--generico", action="store_true",
                    help="(passo 11a) fundo e sprites valem em todo lugar (como antes)")
    ap.add_argument("--contexto",
                    help="(passo 11a) forca o contexto: fXX = fase $XX, mXX = tela do mapa "
                         "(pagina DX $XX), gXX = outra tela (modo do jogo $XX), vazio = nenhum. "
                         "Util para cenas gravadas antes do 11a (lista em tools/fases.csv)")
    args = ap.parse_args()
    if not args.bg and not args.obj:
        ap.error("informe --bg e/ou --obj")
    if args.generico:
        args.fundo = args.sprites = "generico"
    ctx_override = None
    if args.contexto is not None:
        ctx_override = args.contexto.strip().lower()
        import re
        if ctx_override and not re.fullmatch(r"[fmg][0-9a-f]{2}", ctx_override):
            ap.error("--contexto precisa ser fXX, mXX ou gXX (XX em hexadecimal), ex.: f07, m44, g00")

    scene_path = Path(args.scene)
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    tiles = scene["tiles"]
    out_dir = Path(args.out) if args.out else scene_path.parent.parent / "hd"
    out_dir.mkdir(parents=True, exist_ok=True)

    n = None
    layers = []
    base_w, base_h = scene["width"], scene["height"]
    if args.bg:
        img, n = load_painted(args.bg, base_w, base_h)
        layers.append(("bg", img))
    if args.obj:
        img, n = load_painted(args.obj, base_w, base_h, n)
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
    empty_total = []   # passo 7c: (camada, tiles sem pintura)
    still_generic = []  # passo 11a: especificos que ainda tem um generico na pasta
    for layer, img in layers:
        policy = args.fundo if layer == "bg" else args.sprites
        group_of, avisos = output_names(scene, layer, policy, ctx_override)
        for a in avisos:
            print("aviso:", a)
        ref_path = scene_path.with_name(scene_path.stem + f"_{layer}.png")
        reference = Image.open(ref_path).convert("RGBA") if ref_path.exists() else None
        if reference is None:
            print(f"aviso: {ref_path.name} nao encontrado; tiles que aparecem com mais de "
                  f"uma paleta podem sair com cor errada")
        result, hot, skipped = slice_layer(scene, layer, img, n, args.modo, reference,
                                           prefer_painted=not args.todas_votam,
                                           keep_empty=args.manter_vazios,
                                           group_of=group_of)
        if skipped:
            empty_total.append((layer, len(skipped)))
        if args.mapa_conflitos:
            map_path = out_dir / f"conflitos_{layer}.png"
            save_conflict_map(img, hot, map_path)
            print(f"mapa de conflitos: {map_path}")
        for name, (pixels, covered, nconf, tid) in result.items():
            tile = tiles[tid]
            path = out_dir / (name + ".png")
            base = None
            if path.exists() and not args.substituir:
                old = Image.open(path).convert("RGBA")
                if old.size == (size, size):
                    base = pixels_of(old)
                else:
                    print(f"aviso: {path.name} tem {old.size[0]}x{old.size[1]}, "
                          f"recriado em {size}x{size}")
            fallbacks = generic_fallbacks(name)
            if base is None and not args.substituir:
                # Passo 11a: tile novo especifico comeca do que ja vale no jogo
                # (ex.: o generico pintado antes), nao do original.
                for fb in fallbacks:
                    fb_path = out_dir / (fb + ".png")
                    if fb_path.exists():
                        old = Image.open(fb_path).convert("RGBA")
                        if old.size == (size, size):
                            base = pixels_of(old)
                            break
            if fallbacks and (out_dir / (fallbacks[-1] + ".png")).exists():
                still_generic.append(fallbacks[-1])
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
    if still_generic:
        gen = sorted(set(still_generic))
        print(f"{len(gen)} destes tiles tambem existem genericos na pasta (ex.: {gen[0]}.png): "
              f"o generico continua valendo nas outras fases e paletas. Apague-o se nao quiser.")
    for layer, count in empty_total:
        nome = "fundo" if layer == "bg" else "sprites"
        print(f"{count} tiles de {nome} sem pintura: nao gravados (ficam como estavam)")
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
