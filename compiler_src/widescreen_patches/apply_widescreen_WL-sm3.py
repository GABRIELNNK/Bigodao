#!/usr/bin/env python3
"""Apply the exact-ROM Wario Land widescreen patch to generated C sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


class PatchError(RuntimeError):
    pass


def load_manifest(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PatchError(f"cannot read patch manifest: {error}") from error
    if data.get("schema") != "warioland.widescreen-patch" or data.get("version") != 1:
        raise PatchError("unsupported Wario Land patch manifest")
    return data


def verify_rom(rom_path: Path, manifest: dict) -> str:
    data = rom_path.read_bytes()
    expected = manifest["rom"]
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["size"] or digest != expected["sha256"]:
        raise PatchError(
            f"unsupported ROM: size={len(data)} sha256={digest}; "
            f"expected size={expected['size']} sha256={expected['sha256']}"
        )
    return digest


def apply(output: Path, manifest: dict) -> list[dict[str, str]]:
    files = sorted(output.glob("*_funcs_*.c"))
    if manifest["patches"] and not files:
        raise PatchError(f"generated function sources not found: {output}")
    changes = []
    for patch in manifest["patches"]:
        address = patch["generated_address"]
        address_text = address[2:].lower()
        bank = patch.get("generated_bank")
        comment_address = f"{bank.lower()}:{address_text}" if bank else address_text
        expected_matches = patch.get("expected_matches", 1)
        original = patch["original"]
        replacement = patch["replacement"]
        pattern = re.compile(
            rf"(\/\*\s*{re.escape(comment_address)}\s*\*\/\s+){re.escape(original)}",
            re.IGNORECASE,
        )
        matches = []
        for source in files:
            text = source.read_text(encoding="utf-8")
            for match in pattern.finditer(text):
                matches.append((source, text, match))
        if len(matches) != expected_matches:
            if len(matches) == 0:
                already_applied = []
                for source in files:
                    text = source.read_text(encoding="utf-8")
                    if re.search(
                        rf"\/\*\s*{re.escape(comment_address)}\s*\*\/\s+{re.escape(replacement)}",
                        text,
                        re.IGNORECASE,
                    ):
                        already_applied.append(source.name)
                if len(already_applied) == 1:
                    raise PatchError(
                        f"patch already applied at {address} in {already_applied[0]}"
                    )
            raise PatchError(
                f"expected {expected_matches} instruction(s) at {address}, found {len(matches)}"
            )
        patched_files = []
        for source in dict.fromkeys(item[0] for item in matches):
            text = source.read_text(encoding="utf-8")
            updated, count = pattern.subn(rf"\g<1>{replacement}", text)
            if count:
                source.write_text(updated, encoding="utf-8")
                patched_files.append(source.name)
        changes.append({
            "address": address,
            "bank": bank or "00",
            "files": patched_files,
            "occurrences": str(expected_matches),
            "original": original,
            "replacement": replacement,
        })
    return changes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rom", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("patch_manifest.json"))
    args = parser.parse_args()
    try:
        manifest = load_manifest(args.manifest)
        digest = verify_rom(args.rom, manifest)
        changes = apply(args.output, manifest)
    except (OSError, PatchError) as error:
        parser.error(str(error))
    report = {"rom_sha256": digest, "changes": changes}
    (args.output / "warioland-widescreen-patch-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
