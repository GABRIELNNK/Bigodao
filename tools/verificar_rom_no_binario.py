#!/usr/bin/env python3
"""Falha se um binário de release contiver trechos da ROM.

Procura janelas de 32 bytes das ROMs (a cada 64 bytes, sem as de pouca variação,
como preenchimento) em cada arquivo. Em um .apk, confere as bibliotecas nativas.

Uso: python3 tools/verificar_rom_no_binario.py build/wario_land_port app.apk
Saída: 0 = limpo; 1 = achou ROM; 2 = não deu para conferir.
"""

import argparse
import sys
import zipfile
from pathlib import Path

JANELA = 32
PASSO = 64
RAIZ = Path(__file__).resolve().parent.parent
ROMS_PADRAO = [RAIZ / "base_WL-SM3.gb", RAIZ / "hack1.2_WL-SM3.gb"]


def janelas_da_rom(rom: bytes) -> set[bytes]:
    janelas = (rom[i:i + JANELA] for i in range(0, len(rom) - JANELA, PASSO))
    return {j for j in janelas if len(set(j)) >= 8}


def contar_achadas(dados: bytes, janelas: set[bytes]) -> int:
    prefixos = {j[:4] for j in janelas}
    achadas = set()
    visao = memoryview(dados)
    for i in range(len(dados) - JANELA):
        if bytes(visao[i:i + 4]) in prefixos:
            trecho = bytes(visao[i:i + JANELA])
            if trecho in janelas:
                achadas.add(trecho)
    return len(achadas)


def partes_do_arquivo(caminho: Path):
    """Devolve (nome, bytes) de cada parte a conferir."""
    if caminho.suffix.lower() == ".apk":
        with zipfile.ZipFile(caminho) as apk:
            libs = [n for n in apk.namelist() if n.startswith("lib/") and n.endswith(".so")]
            if not libs:
                raise ValueError("APK sem bibliotecas nativas")
            for nome in libs:
                yield f"{caminho}!{nome}", apk.read(nome)
    else:
        yield str(caminho), caminho.read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binarios", nargs="+", type=Path)
    parser.add_argument("--rom", action="append", type=Path, help="ROM de referência (repetível)")
    args = parser.parse_args()

    tabelas = []
    for rom in args.rom or ROMS_PADRAO:
        if not rom.is_file():
            print(f"ROM de referência não encontrada: {rom}", file=sys.stderr)
            return 2
        tabelas.append((rom.name, janelas_da_rom(rom.read_bytes())))

    resultado = 0
    for binario in args.binarios:
        try:
            for nome, dados in partes_do_arquivo(binario):
                for rom_nome, janelas in tabelas:
                    achadas = contar_achadas(dados, janelas)
                    print(f"{nome}: {achadas}/{len(janelas)} janelas de {rom_nome}")
                    if achadas:
                        resultado = 1
        except (OSError, ValueError, zipfile.BadZipFile) as erro:
            print(f"Não deu para conferir {binario}: {erro}", file=sys.stderr)
            return 2

    print("ROM ENCONTRADA" if resultado else "Limpo: nenhum trecho da ROM.")
    return resultado


if __name__ == "__main__":
    raise SystemExit(main())
