#!/usr/bin/env python3
"""
Check that this machine can build and test Marmot, and show what is built.

Required: Python 3.10+, CMake 3.24+, Ninja, and a C++23 compiler (MSVC on
Windows; GCC 14+ or Clang 19+ on Linux). Optional: cargo for the marmot tool,
Emscripten for the WebAssembly build.

Example:
    python scripts/dev.py doctor
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import cargo, console, toolchain
from lib.host import IS_WINDOWS, preset_family
from lib.presets import BUILD_TYPES, BuildTree

_MINIMUM_CMAKE = (3, 24)


def version_of(program: str, *arguments: str) -> str | None:
    found = toolchain.find_build_tool(program)
    if found is None:
        return None
    described = subprocess.run([found, *arguments], capture_output=True, text=True, check=False)
    lines = (described.stdout or described.stderr).strip().splitlines()
    return lines[0] if lines else found


def check_cmake() -> bool:
    described = version_of("cmake", "--version")
    if described is None:
        console.fail("cmake: not found (3.24 or newer is needed)")
        return False
    numbers = tuple(int(part) for part in re.findall(r"\d+", described)[:2])
    if numbers < _MINIMUM_CMAKE:
        console.fail(f"cmake: {described} (3.24 or newer is needed)")
        return False
    console.ok(f"cmake: {described}")
    return True


def check_ninja() -> bool:
    described = version_of("ninja", "--version")
    if described is None:
        console.fail("ninja: not found")
        return False
    console.ok(f"ninja: {described}")
    return True


def check_compiler() -> bool:
    if IS_WINDOWS:
        root = toolchain.visual_studio_root()
        if shutil.which("cl") is not None:
            console.ok("C++ compiler: MSVC, from this Developer Prompt")
            return True
        if root is None:
            console.fail("C++ compiler: no Visual Studio with the C++ x64 tools (install 'Desktop development with C++')")
            return False
        console.ok(f"C++ compiler: MSVC from {root}")
        return True

    chosen = os.environ.get("CXX")
    if chosen:
        if toolchain.supports_cxx23_library(chosen):
            console.ok(f"C++ compiler: CXX={chosen}")
            return True
        console.fail(f"C++ compiler: CXX={chosen} is older than GCC 14 / Clang 19")
        return False
    found = toolchain.linux_cxx()
    if found is None:
        console.fail("C++ compiler: none new enough; install GCC 14+ or Clang 19+, or set CXX")
        return False
    console.ok(f"C++ compiler: {found} (picked when a build tree is first configured)")
    return True


def check_optional() -> None:
    if cargo.is_available():
        console.ok(f"cargo: {version_of('cargo', '--version')}")
    else:
        console.note("cargo: not found; the marmot tool and its tests are skipped")
    if shutil.which("emcc") is not None or os.environ.get("EMSDK"):
        console.ok("emscripten: found")
    else:
        console.note("emscripten: not found; only `dev.py wasm` needs it")


def modified(path: Path) -> str:
    return datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d %H:%M")


def show_builds() -> None:
    print(f"\n{console.Color.BOLD}Build trees ({preset_family()}*){console.Color.RESET}")
    for build_type in BUILD_TYPES:
        tree = BuildTree.select(build_type)
        if not tree.is_configured:
            print(f"  {tree.preset:<22} not configured")
            continue
        built = [path.name for path in (tree.compiler, tree.vm, tree.unit_tests) if path.is_file()]
        stamp = f", marmotc built {modified(tree.compiler)}" if tree.compiler.is_file() else ""
        print(f"  {tree.preset:<22} {', '.join(built) if built else 'nothing built'}{stamp}")

    tool = cargo.newest_binary()
    if tool is None:
        print("  marmot tool            not built")
    else:
        print(f"  marmot tool            {tool.parent.name}, built {modified(tool)}{' (older than its source)' if cargo.is_stale(tool) else ''}")

    installed = shutil.which("marmot")
    print(f"\n  marmot on PATH:  {installed if installed else 'none'}")
    print(f"  MARMOT_PATH:     {os.environ.get('MARMOT_PATH', '(unset)')}")


def main(argv: list[str]) -> int:
    argparse.ArgumentParser(description="Check the toolchain and show what is built.").parse_args(argv)
    console.ok(f"python: {sys.version.split()[0]}")
    required = [check_cmake(), check_ninja(), check_compiler()]
    check_optional()
    show_builds()
    return 0 if all(required) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
