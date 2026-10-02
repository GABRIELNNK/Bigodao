#!/usr/bin/env python3
"""
inject_text.py - Busca e substitui textos nos arquivos gerados em
compiler_src/game_code (ou qualquer outra pasta do projeto).

Modos de uso:

1) Par unico de busca/substituicao:
   python3 tools/inject_text.py --search "GB_OAM_BASE" --replace "GB_OAM_BASE_NOVO" \
       --dir compiler_src/game_code --ext .c,.h

2) Varias substituicoes via arquivo de mapeamento (JSON):
   python3 tools/inject_text.py --map replacements.json --dir compiler_src/game_code

   onde replacements.json e um objeto simples:
   {
       "texto antigo 1": "texto novo 1",
       "texto antigo 2": "texto novo 2"
   }

3) Busca/substituicao com regex (grupos de captura sao suportados no --replace):
   python3 tools/inject_text.py --search "GB_OAM_BASE \\+ 0x([0-9a-f]+)" \
       --replace "GB_OAM_BASE + 0x\\1 /* patched */" --regex

Por padrao roda em modo "dry-run" apenas listando o que seria alterado.
Use --apply para gravar as mudancas de fato. Um backup .bak e criado
automaticamente na primeira gravacao de cada arquivo (desative com --no-backup).
"""

import argparse
import json
import re
import sys
from pathlib import Path


def load_replacements(args: argparse.Namespace) -> list[tuple[str, str]]:
    """Retorna lista de pares (busca, substituicao) na ordem de aplicacao."""
    pairs: list[tuple[str, str]] = []

    if args.map:
        data = json.loads(Path(args.map).read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("O arquivo de mapeamento deve conter um objeto JSON {busca: substituicao}")
        pairs.extend(data.items())

    if args.search is not None:
        if args.replace is None:
            raise ValueError("--search requer --replace")
        pairs.append((args.search, args.replace))

    if not pairs:
        raise ValueError("Informe --map e/ou --search/--replace com ao menos uma substituicao")

    return pairs


def iter_target_files(root: Path, extensions: set[str]) -> list[Path]:
    files = [p for p in root.rglob("*") if p.is_file() and p.suffix in extensions]
    files.sort()
    return files


def apply_replacements(text: str, pairs: list[tuple[str, str]], use_regex: bool):
    total_hits = 0
    per_pair_hits: list[int] = []
    for search, replace in pairs:
        if use_regex:
            text, n = re.subn(search, replace, text)
        else:
            n = text.count(search)
            if n:
                text = text.replace(search, replace)
        per_pair_hits.append(n)
        total_hits += n
    return text, total_hits, per_pair_hits


def main() -> int:
    parser = argparse.ArgumentParser(description="Injetor de busca/substituicao de texto nos arquivos do projeto")
    parser.add_argument("--dir", default="compiler_src/game_code", help="Pasta raiz a varrer (default: compiler_src/game_code)")
    parser.add_argument("--ext", default=".c,.h", help="Extensoes de arquivo a considerar, separadas por virgula (default: .c,.h)")
    parser.add_argument("--map", help="Arquivo JSON com pares {busca: substituicao}")
    parser.add_argument("--search", help="Texto (ou regex, com --regex) a buscar")
    parser.add_argument("--replace", help="Texto de substituicao para --search")
    parser.add_argument("--regex", action="store_true", help="Trata --search (e as chaves do --map) como expressao regular")
    parser.add_argument("--apply", action="store_true", help="Grava as alteracoes. Sem essa flag, apenas simula (dry-run)")
    parser.add_argument("--no-backup", action="store_true", help="Nao criar arquivo .bak ao gravar")
    args = parser.parse_args()

    root = Path(args.dir)
    if not root.exists():
        print(f"Pasta nao encontrada: {root}", file=sys.stderr)
        return 1

    extensions = {e if e.startswith(".") else f".{e}" for e in args.ext.split(",") if e}

    try:
        pairs = load_replacements(args)
    except ValueError as exc:
        print(f"Erro: {exc}", file=sys.stderr)
        return 1

    files = iter_target_files(root, extensions)
    if not files:
        print(f"Nenhum arquivo com extensoes {sorted(extensions)} encontrado em {root}")
        return 0

    changed_files = 0
    total_hits = 0

    for path in files:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        new_text, hits, per_pair_hits = apply_replacements(original, pairs, args.regex)

        if hits == 0:
            continue

        changed_files += 1
        total_hits += hits
        status = "APLICADO" if args.apply else "SIMULADO"
        print(f"[{status}] {path}: {hits} ocorrencia(s)")
        for (search, _replace), n in zip(pairs, per_pair_hits):
            if n:
                preview = search if len(search) <= 60 else search[:57] + "..."
                print(f"    - '{preview}' x{n}")

        if args.apply:
            if not args.no_backup:
                backup_path = path.with_suffix(path.suffix + ".bak")
                if not backup_path.exists():
                    backup_path.write_text(original, encoding="utf-8", errors="surrogateescape")
            path.write_text(new_text, encoding="utf-8", errors="surrogateescape")

    mode = "Alteracoes aplicadas" if args.apply else "Simulacao (use --apply para gravar)"
    print(f"\n{mode}: {changed_files} arquivo(s) afetado(s), {total_hits} ocorrencia(s) no total.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
