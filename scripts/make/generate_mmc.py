#!/usr/bin/env python3
"""Generate project-private C++ headers and tables from the language-neutral .mmc registry."""

from __future__ import annotations

import argparse
import json
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT

REGISTRY = REPO_ROOT / "format" / "mmc" / "registry.json"
BANNER = "// Generated from format/mmc/registry.json. Do not edit.\n// Regenerate: python scripts/dev.py generate-mmc\n\n"


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


def format_header(registry: dict) -> str:
    wire = registry["wire"]
    magic = ", ".join(f"0x{byte:02X}u" for byte in wire["magic"])
    offsets = "\n".join(f"\tinline constexpr std::size_t {name.upper()}_OFFSET = {offset}u;"
                        for name, offset in wire["header_offsets"].items())
    crc32 = wire["crc32"]
    foreign = wire["call_foreign"]
    kinds = "\n".join(f"\tinline constexpr uint8_t FFI_{direction}_{name.upper()} = {kind}u;"
                      for direction in ("ARGUMENT", "RETURN")
                      for name, kind in foreign[f"{direction.lower()}_kinds"].items())
    return BANNER + f"""#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace MmcWire
{{
	inline constexpr std::array<uint8_t, {len(wire["magic"])}u> MAGIC{{ {magic} }};
	inline constexpr uint32_t FORMAT_MARKER = {registry["format_marker"]}u;
	inline constexpr std::size_t HEADER_SIZE = {wire["header_size"]}u;
{offsets}
	inline constexpr uint32_t SOURCE_FILES_EMBEDDED_FLAG = {wire["source_files_embedded_flag"]}u;

	inline constexpr uint32_t CRC32_POLYNOMIAL = 0x{crc32["polynomial"]:08X}u;
	inline constexpr uint32_t CRC32_INITIAL = 0x{crc32["initial"]:08X}u;
	inline constexpr uint32_t CRC32_FINAL_XOR = 0x{crc32["final_xor"]:08X}u;

	inline constexpr int FFI_ARGUMENT_BITS = {foreign["argument_bits"]};
	inline constexpr uint8_t FFI_ARGUMENT_MASK = 0x{foreign["argument_mask"]:02X}u;
	inline constexpr uint8_t FFI_RETURN_KIND_MASK = 0x{foreign["return_kind_mask"]:02X}u;
	inline constexpr uint8_t FFI_POINTER_ELEMENTS_FLAG = 0x{foreign["pointer_elements_flag"]:02X}u;
{kinds}

	inline constexpr int MAX_UNION_TAG = {wire["max_union_tag"]};
	inline constexpr int MAX_ARRAY_SIZE = {wire["max_array_size"]};
	inline constexpr char MODULE_SEPARATOR = {wire["module_separator"]!r};
	inline constexpr std::string_view MAIN_PROCEDURE_PREFIX = {json.dumps(wire["main_procedure_prefix"])};
	inline constexpr std::string_view MODULE_BOOTSTRAP_PREFIX = {json.dumps(wire["module_bootstrap_prefix"])};
}}
"""


def generated_files(registry: dict) -> dict[Path, str]:
    compiler = REPO_ROOT / "projects" / "marmotc" / "src" / "Bytecode"
    vm = REPO_ROOT / "projects" / "marmotvm" / "src" / "Bytecode"
    instructions = opcodes(registry)
    header = format_header(registry)
    return {
        compiler / "Executable" / "OpCodes.def": instructions,
        compiler / "Builtins" / "Builtins.def": compiler_builtins(registry),
        compiler / "Format" / "Format.h": header,
        vm / "Executable" / "OpCodes.def": instructions,
        vm / "Builtins" / "Builtins.def": vm_builtins(registry),
        vm / "Format" / "Format.h": header,
    }


def stale_files(files: dict[Path, str]) -> list[Path]:
    return [path for path, content in files.items()
            if not path.is_file() or path.read_text(encoding="utf-8") != content]


def check_generated() -> None:
    stale = stale_files(generated_files(load_registry()))
    if stale:
        names = ", ".join(path.relative_to(REPO_ROOT).as_posix() for path in stale)
        raise AssertionError(f"Stale .mmc headers or tables: {names}. Run python scripts/dev.py generate-mmc")


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
        print("Generated .mmc headers and tables are up to date.")
        return 0
    for path in stale:
        path.write_text(files[path], encoding="utf-8", newline="\n")
        print(f"Generated: {path.relative_to(REPO_ROOT).as_posix()}")
    print("Generated .mmc headers and tables are up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
