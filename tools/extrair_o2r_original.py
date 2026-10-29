#!/usr/bin/env python3
"""
Gera um .o2r a partir da ROM ORIGINAL de Wario Land: Super Mario Land 3, sem
aplicar o patch IPS do DX. Script standalone, para teste: não precisa do jogo,
do manifesto JSON nem do .ips.

O pacote tem o mesmo formato do wario.o2r que o jogo cria (asset_extractor.hpp,
formato 2): um ZIP sem compressão com a ROM fatiada em pedaços contíguos.

    rom_header.bin            0x0100-0x014F (só informativo)
    data/bank0.bin            0x00000-0x03FFF
    gfx/vram_tiles.bin        0x04000-0x0BFFF
    gfx/sprite_tiles.bin      0x0C000-0x13FFF
    audio/wave_tables.bin     0x14000-0x17FFF
    data/logic_banks.bin      0x18000-fim
    bigodao/o2r_info.txt      formato, tamanho, origem e SHA-256

A diferença está no conteúdo: aqui vai a ROM original (512 KB, só DMG), e o
o2r_info.txt diz "patched=0". Por isso a saída padrão é wario_original.o2r e
não wario.o2r. O executável atual foi recompilado a partir da ROM do DX, então
não use este pacote no lugar do wario.o2r do jogo.

Uso:
    python3 extrair_o2r_original.py rom.gb --conferir      # mostra o que seria gerado, sem gravar
    python3 extrair_o2r_original.py rom.gb                 # grava wario_original.o2r
    python3 extrair_o2r_original.py rom.gb -o saida.o2r
    python3 extrair_o2r_original.py --verificar saida.o2r [rom.gb]
        # remonta a ROM a partir do pacote e confere (com a ROM, compara byte a byte)

A ROM precisa ter o SHA-256 esperado (o mesmo que o launcher pede). Com
--forcar, aceita outra ROM, mas recusa uma que já tenha o patch DX.
"""
import argparse
import hashlib
import io
import os
import sys
import zipfile
from pathlib import Path

EXPECTED_ROM_SHA256 = "ac1682f17abcf590311a233289ee325214c2d71ab3a5aa175004002d85075e56"
FORMAT_VERSION = 2
INFO_NAME = "bigodao/o2r_info.txt"
MIN_ROM_SIZE = 0x18000

# Mesma ordem e mesmos nomes do asset_extractor.hpp. end = None: até o fim.
SEGMENTS = [
    ("data/bank0.bin",        0x00000, 0x04000),
    ("gfx/vram_tiles.bin",    0x04000, 0x0C000),
    ("gfx/sprite_tiles.bin",  0x0C000, 0x14000),
    ("audio/wave_tables.bin", 0x14000, 0x18000),
    ("data/logic_banks.bin",  0x18000, None),
]

# Data fixa nas entradas (a mesma do C++: 0x5401 / 0x3A00 = 2022-01-01 07:16),
# para que a mesma ROM gere sempre o mesmo arquivo.
ZIP_DATE = (2022, 1, 1, 7, 16, 0)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def header_info(rom):
    """Título, flag CGB, tamanho declarado e checksum do cabeçalho."""
    title = rom[0x134:0x143].split(b"\0")[0].decode("ascii", "replace")
    cgb = rom[0x143]
    cart = rom[0x147]
    rom_size_code = rom[0x148]
    chk = 0
    for b in rom[0x134:0x14D]:
        chk = (chk - b - 1) & 0xFF
    return {
        "title": title,
        "cgb_flag": cgb,
        "cart_type": cart,
        "declared_size": (32 * 1024) << rom_size_code if rom_size_code < 9 else None,
        "header_ok": chk == rom[0x14D],
    }


def looks_patched(rom, info):
    """O patch DX deixa a ROM com 1 MB e marca suporte a CGB (0x80/0xC0)."""
    return len(rom) > 512 * 1024 or info["cgb_flag"] in (0x80, 0xC0)


def build_entries(rom, rom_sha):
    entries = [("rom_header.bin", rom[0x100:0x150])]
    for name, begin, end in SEGMENTS:
        entries.append((name, rom[begin:end if end is not None else len(rom)]))
    info = (
        f"format={FORMAT_VERSION}\n"
        f"rom_size={len(rom)}\n"
        "content=Wario Land: Super Mario Land 3 (original, sem patch IPS)\n"
        "patched=0\n"
        f"rom_sha256={rom_sha}\n"
    )
    entries.append((INFO_NAME, info.encode("ascii")))
    return entries


def write_zip(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_STORED) as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0x20     # arquivo comum (como no C++)
            zi.create_system = 0
            z.writestr(zi, data)
    return buf.getvalue()


def read_info(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def rebuild_rom(o2r_bytes):
    """Mesma lógica do AssetExtractor::load_from. Devolve (rom, info) ou erro."""
    with zipfile.ZipFile(io.BytesIO(o2r_bytes)) as z:
        names = set(z.namelist())
        if INFO_NAME not in names:
            raise ValueError("sem bigodao/o2r_info.txt (pacote de versão antiga)")
        info = read_info(z.read(INFO_NAME).decode("ascii", "replace"))
        if info.get("format") != str(FORMAT_VERSION):
            raise ValueError(f"formato {info.get('format')} (esperado {FORMAT_VERSION})")
        rom = bytearray()
        for name, begin, end in SEGMENTS:
            if name not in names:
                raise ValueError(f"falta {name}")
            data = z.read(name)       # confere o CRC
            if end is not None and len(data) != end - begin:
                raise ValueError(f"{name} com {len(data)} bytes (esperado {end - begin})")
            rom += data
        if str(len(rom)) != info.get("rom_size"):
            raise ValueError(f"tamanho {len(rom)} diferente de rom_size={info.get('rom_size')}")
    return bytes(rom), info


def cmd_build(args):
    rom_path = Path(args.rom)
    rom = rom_path.read_bytes()
    rom_sha = sha256(rom)
    if len(rom) <= MIN_ROM_SIZE:
        print(f"[ERRO] ROM pequena demais ({len(rom)} bytes)")
        return 1
    info = header_info(rom)

    print(f"ROM:      {rom_path}")
    print(f"Tamanho:  {len(rom)} bytes ({len(rom) // 1024} KB)")
    print(f"Título:   {info['title']!r}   CGB: 0x{info['cgb_flag']:02X}   "
          f"cartucho: 0x{info['cart_type']:02X}   cabeçalho: {'ok' if info['header_ok'] else 'checksum errado'}")
    print(f"SHA-256:  {rom_sha}")

    if looks_patched(rom, info):
        print("[ERRO] Esta ROM parece já ter o patch DX (1 MB ou flag CGB). Use a ROM original.")
        return 1
    if rom_sha != EXPECTED_ROM_SHA256:
        if not args.forcar:
            print(f"[ERRO] SHA-256 diferente do esperado ({EXPECTED_ROM_SHA256}).\n"
                  "       Use --forcar para gerar mesmo assim.")
            return 1
        print("[aviso] SHA-256 diferente do esperado; seguindo por causa de --forcar.")
    else:
        print("[ok]     SHA-256 confere com a ROM original esperada.")
    if info["declared_size"] and info["declared_size"] != len(rom):
        print(f"[aviso] O cabeçalho declara {info['declared_size']} bytes.")

    entries = build_entries(rom, rom_sha)
    print("\nConteúdo do pacote:")
    off = None
    for name, data in entries:
        rng = ""
        for sname, begin, end in SEGMENTS:
            if sname == name:
                e = end if end is not None else len(rom)
                rng = f"0x{begin:05X}-0x{e - 1:05X}"
        print(f"  {name:<24} {len(data):>8} bytes  {rng}")

    z = write_zip(entries)
    back, _ = rebuild_rom(z)
    if back != rom:
        print("[ERRO] A ROM remontada do pacote não bate com a original. Nada foi gravado.")
        return 1
    print(f"\n[ok]     Ida e volta conferida: o pacote remonta a ROM byte a byte ({len(z)} bytes de ZIP).")

    out = Path(args.output) if args.output else Path("wario_original.o2r")
    if out.name.lower() == "wario.o2r":
        print("[aviso] O nome wario.o2r é o pacote do jogo (DX). O executável atual não roda a ROM original.")
    if args.conferir:
        print(f"Conferência: nada foi gravado. Rode sem --conferir para gravar {out}.")
        return 0

    tmp = out.with_name(out.name + ".tmp")
    tmp.write_bytes(z)
    os.replace(tmp, out)
    print(f"[ok]     Gravado: {out.resolve()}")
    return 0


def cmd_verify(args):
    o2r = Path(args.verificar)
    try:
        rom, info = rebuild_rom(o2r.read_bytes())
    except (ValueError, zipfile.BadZipFile, KeyError) as e:
        print(f"[ERRO] {o2r}: {e}")
        return 1
    print(f"[ok]     {o2r}: formato {info.get('format')}, {len(rom)} bytes, "
          f"patched={info.get('patched', '?')}")
    print(f"         {info.get('content', '')}")
    rom_sha = sha256(rom)
    print(f"         SHA-256 da ROM remontada: {rom_sha}")
    if info.get("rom_sha256") and info["rom_sha256"] != rom_sha:
        print("[ERRO] Diferente do rom_sha256 gravado no pacote (alguma fatia foi trocada).")
        return 1
    if args.rom:
        ref = Path(args.rom).read_bytes()
        if ref == rom:
            print(f"[ok]     Igual a {args.rom} byte a byte.")
        else:
            diff = next((i for i, (a, b) in enumerate(zip(ref, rom)) if a != b), min(len(ref), len(rom)))
            print(f"[ERRO] Diferente de {args.rom} a partir do byte 0x{diff:05X}.")
            return 1
    return 0


def main():
    p = argparse.ArgumentParser(description="Gera um .o2r da ROM original (sem o patch IPS).")
    p.add_argument("rom", nargs="?", help="ROM original (.gb)")
    p.add_argument("-o", "--output", help="arquivo de saída (padrão: wario_original.o2r)")
    p.add_argument("--conferir", action="store_true", help="mostra o que seria gerado, sem gravar")
    p.add_argument("--forcar", action="store_true", help="aceita ROM com outro SHA-256")
    p.add_argument("--verificar", metavar="O2R", help="confere um .o2r já gerado")
    args = p.parse_args()
    if args.verificar:
        return cmd_verify(args)
    if not args.rom:
        p.print_help()
        return 1
    return cmd_build(args)


if __name__ == "__main__":
    sys.exit(main())
