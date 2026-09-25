#!/usr/bin/env python3
"""
Run the C++ unit tests (MarmotUnitTests, Catch2).

By default every test runs through CTest; --tag runs the Catch2 binary directly
with a tag expression, and --regex narrows CTest to matching names.

Examples:
    python scripts/dev.py unit
    python scripts/dev.py unit --tag "[runtime]"
    python scripts/dev.py unit --regex TypeChecker
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import toolchain
from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the C++ unit tests.")
    add_build_arguments(parser)
    parser.add_argument("--tag", default="", help="A Catch2 tag expression, e.g. \"[runtime]\"; runs MarmotUnitTests directly.")
    parser.add_argument("--regex", default="", help="A CTest name regex, e.g. TypeChecker.")
    args = parser.parse_args(argv)

    tree = BuildTree.from_args(args)
    if not tree.unit_tests.is_file():
        raise SystemExit(f"{tree.unit_tests} is not built. Build it with: python scripts/dev.py build unit --build {tree.build_type}")

    if args.tag:
        return toolchain.run([str(tree.unit_tests), args.tag])
    ctest = ["ctest", "--test-dir", str(tree.binary_dir), "--output-on-failure", *(["-R", args.regex] if args.regex else [])]
    return toolchain.run(ctest, environment=toolchain.build_environment())


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
