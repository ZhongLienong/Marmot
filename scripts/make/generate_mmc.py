#!/usr/bin/env python3
"""Generate project-private C++ tables from the language-neutral .mmc registry."""

from __future__ import annotations

import argparse
import json
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT

REGISTRY = REPO_ROOT / "format" / "mmc" / "registry-v13.json"
BANNER = "// Generated from format/mmc/registry-v13.json. Do not edit.\n// Regenerate: python scripts/dev.py generate-mmc\n\n"


def load_registry() -> dict:
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    for table in ("opcodes", "builtins"):
        entries = registry[table]
        if [entry["id"] for entry in entries] != list(range(len(entries))):
            raise ValueError(f"{table}: IDs must be consecutive and listed in encoding order")
        names = [entry["name"] for entry in entries]
        if len(set(names)) != len(names):
            raise ValueError(f"{table}: names must be unique")
    if len(registry["opcodes"]) > 256 or any(entry["length"] < 1 for entry in registry["opcodes"]):
        raise ValueError("opcodes must fit in one byte and have positive instruction lengths")
    return registry


def opcodes(registry: dict) -> str:
    lines = []
    category = None
    for opcode in registry["opcodes"]:
        if opcode["category"] != category:
            if lines:
                lines.append("")
            category = opcode["category"]
            lines.extend("// " + line for line in textwrap.wrap(category, width=96))
        line = f'MARMOT_OPCODE({opcode["name"]}, {opcode["length"]})'
        if "description" in opcode:
            line += " // " + opcode["description"]
        lines.append(line)
    return BANNER + "\n".join(lines) + "\n"


def compiler_builtins(registry: dict) -> str:
    return BANNER + "\n".join(f'MARMOT_BUILTIN({builtin["name"]})' for builtin in registry["builtins"]) + "\n"


def vm_builtins(registry: dict) -> str:
    lines = []
    for builtin in registry["builtins"]:
        arguments = ", ".join("VmFFIArgumentKind::" + kind for kind in builtin["arguments"])
        kinds = f"MakeVmFFIArgKinds({arguments})" if arguments else "{}"
        lines.append(f'MARMOT_BUILTIN({builtin["name"]}, {kinds}, VmFFIReturnKind::{builtin["returns"]})')
    return BANNER + "\n".join(lines) + "\n"


def generated_files(registry: dict) -> dict[Path, str]:
    compiler = REPO_ROOT / "projects" / "marmotc" / "src" / "Bytecode"
    vm = REPO_ROOT / "projects" / "marmotvm" / "src" / "Bytecode"
    instructions = opcodes(registry)
    return {
        compiler / "Executable" / "OpCodes.def": instructions,
        compiler / "Builtins" / "Builtins.def": compiler_builtins(registry),
        vm / "Executable" / "OpCodes.def": instructions,
        vm / "Builtins" / "Builtins.def": vm_builtins(registry),
    }


def stale_files(files: dict[Path, str]) -> list[Path]:
    return [path for path, content in files.items()
            if not path.is_file() or path.read_text(encoding="utf-8") != content]


def check_generated() -> None:
    stale = stale_files(generated_files(load_registry()))
    if stale:
        names = ", ".join(path.relative_to(REPO_ROOT).as_posix() for path in stale)
        raise AssertionError(f"Stale .mmc tables: {names}. Run python scripts/dev.py generate-mmc")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check generated files without changing them.")
    args = parser.parse_args(argv)
    files = generated_files(load_registry())
    stale = stale_files(files)
    if args.check:
        for path in stale:
            print(f"Stale: {path.relative_to(REPO_ROOT).as_posix()}")
        if stale:
            print("Run python scripts/dev.py generate-mmc")
            return 1
        print("Generated .mmc tables are up to date.")
        return 0
    for path in stale:
        path.write_text(files[path], encoding="utf-8", newline="\n")
        print(f"Generated: {path.relative_to(REPO_ROOT).as_posix()}")
    print("Generated .mmc tables are up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
