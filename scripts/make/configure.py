#!/usr/bin/env python3
"""
Configure a build tree with its CMake preset.

A build is configured on its first use anyway; this is for reconfiguring:
after changing CMakeLists.txt options, to switch compiler (--fresh with CXX set),
or to include the unit tests in a Release tree.

Examples:
    python scripts/dev.py configure
    python scripts/dev.py configure --build Release --unit-tests
    CXX=clang++-19 python scripts/dev.py configure --fresh
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Configure a build tree with its CMake preset.")
    add_build_arguments(parser)
    parser.add_argument("--fresh", action="store_true", help="Discard the existing CMake cache, and with it the chosen compiler.")
    parser.add_argument("--unit-tests", action="store_true", help="Include the unit tests in a Release tree.")
    args = parser.parse_args(argv)
    return BuildTree.from_args(args).configure(with_unit_tests=args.unit_tests, fresh=args.fresh)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
