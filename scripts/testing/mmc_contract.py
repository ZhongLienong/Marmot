#!/usr/bin/env python3
"""Check the frozen version-13 .mmc fixtures against marmotc and marmotvm."""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT, checkout_environment
from lib.presets import BuildTree, add_build_arguments
from make import generate_mmc


CONTRACT_DIR = REPO_ROOT / "format" / "mmc"
HEADER = struct.Struct("<4sIHHHHIQI")


class Cursor:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, count: int) -> bytes:
        end = self.offset + count
        if end > len(self.data):
            raise ValueError(f"truncated payload at byte {self.offset}")
        result = self.data[self.offset:end]
        self.offset = end
        return result

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def string(self) -> bytes:
        return self.take(self.u32())


class Payload:
    def __init__(self, data: bytes) -> None:
        self.input = Cursor(data)
        self.output = bytearray()

    def u8(self) -> int:
        value = self.input.u8()
        self.output.append(value)
        return value

    def u32(self) -> int:
        value = self.input.u32()
        self.output.extend(struct.pack("<I", value))
        return value

    def raw(self, count: int) -> None:
        self.output.extend(self.input.take(count))

    def string(self, *, path: bool = False) -> None:
        value = self.input.string()
        if path and value:
            source = Path(value.decode("utf-8"))
            value = source.resolve().relative_to(REPO_ROOT.resolve()).as_posix().encode("utf-8")
        self.output.extend(struct.pack("<I", len(value)))
        self.output.extend(value)


def normalize_artifact(blob: bytes) -> bytes:
    """Replace only serialized checkout paths, then update size and CRC."""
    if len(blob) < HEADER.size:
        raise ValueError("truncated .mmc header")
    magic, version, major, minor, patch, reserved, flags, size, crc = HEADER.unpack_from(blob)
    if magic != b"MBC\0" or version != 13:
        raise ValueError("expected a version-13 .mmc artifact")
    original = blob[HEADER.size:]
    if len(original) != size or zlib.crc32(original) != crc:
        raise ValueError("invalid original payload size or CRC")

    payload = Payload(original)
    payload.string(path=True)  # entry file
    for _ in range(payload.u32()):  # string pool
        payload.string()
    for _ in range(payload.u32()):  # globals
        payload.string()
    for _ in range(payload.u32()):  # procedures
        payload.string()  # name
        payload.string(path=True)  # source path
        payload.raw(payload.u32())  # bytecode
        payload.raw(payload.u32() * 8)  # (line, instruction count) pairs
    for _ in range(payload.u32()):  # native libraries
        payload.string()  # library name
        for _ in range(payload.u32()):
            payload.string()  # symbol
        for _ in range(payload.u32()):
            payload.string(path=True)  # hint directory
        payload.u8()  # thread safe
        if payload.u8():  # checksum present
            payload.string()
    if flags & 1:
        for _ in range(payload.u32()):  # embedded source files
            payload.string(path=True)
            for _ in range(payload.u32()):
                payload.string()  # source line
    if payload.input.offset != len(original):
        raise ValueError(f"unparsed payload bytes at offset {payload.input.offset}")

    normalized = bytes(payload.output)
    header = HEADER.pack(magic, version, major, minor, patch, reserved, flags,
                         len(normalized), zlib.crc32(normalized))
    return header + normalized


def source_registry(side: Path, *, signatures: bool) -> dict[str, object]:
    opcode_source = (side / "Executable/OpCodes.def").read_text(encoding="utf-8")
    builtin_source = (side / "Builtins/Builtins.def").read_text(encoding="utf-8")
    builtin_header = (side / "Builtins/BuiltinTable.h").read_text(encoding="utf-8")
    opcodes = [
        {"id": index, "name": name, "length": int(length)}
        for index, (name, length) in enumerate(re.findall(r"^MARMOT_OPCODE\((\w+),\s*(\d+)\)", opcode_source, re.MULTILINE))
    ]
    builtins = []
    for line in builtin_source.splitlines():
        if not line.startswith("MARMOT_BUILTIN("):
            continue
        body = line.removeprefix("MARMOT_BUILTIN(").removesuffix(")")
        if not signatures:
            builtins.append({"id": len(builtins), "name": body.strip()})
            continue
        name, remainder = body.split(",", 1)
        arguments, result = remainder.rsplit(",", 1)
        builtins.append({
            "id": len(builtins),
            "name": name.strip(),
            "arguments": re.findall(r"(?:Vm)?FFIArgumentKind::(\w+)", arguments),
            "returns": re.fullmatch(r"\s*(?:Vm)?FFIReturnKind::(\w+)\s*", result).group(1),
        })
    abi_version = re.search(r"ABI_VERSION\s*=\s*(\d+)", builtin_header)
    if abi_version is None:
        raise AssertionError("builtin ABI version is missing")
    return {"builtin_abi_version": int(abi_version.group(1)),
            "opcodes": opcodes, "builtins": builtins}


def check_registry() -> None:
    generate_mmc.check_generated()
    frozen = json.loads((CONTRACT_DIR / "registry-v13.json").read_text(encoding="utf-8"))
    for name, side, version_name in (
        ("marmotc", REPO_ROOT / "projects/marmotc/src/Bytecode", "MbcFormatVersion"),
        ("marmotvm", REPO_ROOT / "projects/marmotvm/src/Bytecode", "VmMbcFormatVersion"),
    ):
        expected = {
            "builtin_abi_version": frozen["builtin_abi_version"],
            "opcodes": [{key: opcode[key] for key in ("id", "name", "length")} for opcode in frozen["opcodes"]],
            "builtins": frozen["builtins"] if name == "marmotvm" else
                        [{key: builtin[key] for key in ("id", "name")} for builtin in frozen["builtins"]],
        }
        if source_registry(side, signatures=name == "marmotvm") != expected:
            raise AssertionError(f"{name} opcode or builtin table differs from the frozen version-13 registry")
        format_header = (side / "Format/Format.h").read_text(encoding="utf-8")
        version = re.search(rf"{version_name}\s*=\s*(\d+)u", format_header)
        if version is None or int(version.group(1)) != frozen["format_version"]:
            raise AssertionError(f"{name} format version differs from the frozen registry")


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=REPO_ROOT, env=checkout_environment(), capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=30, check=False)


def native_library(tree: BuildTree) -> Path:
    matches = [path for path in tree.out_dir.glob("*marmot_test_native*")
               if path.suffix.lower() in {".dll", ".so", ".dylib"}]
    if len(matches) != 1:
        raise AssertionError(f"expected one native FFI test library in {tree.out_dir}; build the unit target")
    return matches[0]


def check_valid(tree: BuildTree, fixture: dict[str, object]) -> None:
    name = str(fixture["name"])
    artifact = CONTRACT_DIR / "fixtures" / f"{name}.mmc"
    baseline = artifact.read_bytes()
    if normalize_artifact(baseline) != baseline:
        raise AssertionError(f"{name}: committed artifact contains checkout-specific paths")

    with tempfile.TemporaryDirectory(prefix="marmot-mmc-") as directory:
        fresh = Path(directory) / f"{name}.mmc"
        command = [str(tree.require_compiler()), "build", str(fixture["source"]),
                   "-o", str(fresh), "--quiet"]
        if fixture.get("embed_sources"):
            command.append("--embed-sources")
        compiled = run(command)
        if compiled.returncode != 0:
            raise AssertionError(f"{name}: compilation failed:\n{compiled.stdout}{compiled.stderr}")
        if normalize_artifact(fresh.read_bytes()) != baseline:
            raise AssertionError(f"{name}: compiler output differs from frozen .mmc bytes")

    command = [str(tree.vm), "run", str(artifact)]
    if fixture.get("native_library"):
        command.extend(["--library", f"marmot_test_native={native_library(tree)}"])
    executed = run(command)
    if executed.returncode != fixture["exit_code"]:
        raise AssertionError(f"{name}: exit {executed.returncode}, expected {fixture['exit_code']}:\n{executed.stdout}{executed.stderr}")
    if "stdout" in fixture and executed.stdout != fixture["stdout"]:
        raise AssertionError(f"{name}: stdout differs: {executed.stdout!r}")
    for expected in fixture.get("stdout_contains", []):
        if expected not in executed.stdout:
            raise AssertionError(f"{name}: stdout does not contain {expected!r}")
    print(f"[OK] {name}: compiler bytes and VM behavior")


def check_invalid(tree: BuildTree, fixture: dict[str, object]) -> None:
    name = str(fixture["name"])
    artifact = CONTRACT_DIR / "fixtures" / f"{name}.mmc"
    executed = run([str(tree.vm), "run", str(artifact)])
    if executed.returncode != 1 or fixture["stderr_contains"] not in executed.stderr:
        raise AssertionError(f"{name}: VM did not reject the artifact as expected:\n{executed.stdout}{executed.stderr}")
    print(f"[OK] {name}: VM rejection")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check the frozen .mmc contract and fixtures.")
    add_build_arguments(parser)
    args = parser.parse_args(argv)
    tree = BuildTree.from_args(args)
    if not tree.vm.is_file():
        raise SystemExit(f"{tree.vm} is not built")
    manifest = json.loads((CONTRACT_DIR / "fixtures.json").read_text(encoding="utf-8"))
    check_registry()
    for fixture in manifest["valid"]:
        check_valid(tree, fixture)
    for fixture in manifest["invalid"]:
        check_invalid(tree, fixture)
    print("[OK] version-13 .mmc contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
