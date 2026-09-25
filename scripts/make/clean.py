#!/usr/bin/env python3
"""
Delete build output.

By default the build tree of --build/--preset goes; --all takes every CMake
build tree, the WebAssembly build and the marmot tool's cargo target.

Examples:
    python scripts/dev.py clean
    python scripts/dev.py clean --build Release
    python scripts/dev.py clean --all
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import console
from lib.host import REPO_ROOT, TOOL_DIR
from lib.presets import BuildTree, add_build_arguments

WASM_BUILD_DIR = REPO_ROOT / "build-wasm"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Delete build output.")
    add_build_arguments(parser)
    parser.add_argument("--all", action="store_true", help="Every build tree, the WebAssembly build and the tool's cargo target.")
    args = parser.parse_args(argv)

    doomed = [REPO_ROOT / "out" / "build", WASM_BUILD_DIR, TOOL_DIR / "target"] if args.all else [BuildTree.from_args(args).binary_dir]
    for directory in doomed:
        if directory.is_dir():
            shutil.rmtree(directory)
            console.ok(f"removed {directory}")
        else:
            console.note(f"nothing at {directory}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
