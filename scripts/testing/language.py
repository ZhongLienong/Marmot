#!/usr/bin/env python3
"""
The language suite: every .mmt under test/, run by `marmot test` against this
checkout's compiler and VM. docs/testing.md says what passing means.

Usage:
    python scripts/dev.py test                          # every test
    python scripts/dev.py test --category closure       # tests whose path contains 'closure'
    python scripts/dev.py test --pattern loop           # file names containing 'loop'
    python scripts/dev.py test --test closure/simple    # one test
    python scripts/dev.py test --build Debug            # another build
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import cargo, console
from lib.host import REPO_ROOT, removing_new_files
from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the Marmot language suite through `marmot test`.")
    add_build_arguments(parser)
    parser.add_argument("--category", help="Only tests whose path contains this (e.g. closure)")
    parser.add_argument("--pattern", help="Only tests whose file name contains this")
    parser.add_argument("--test", help="One test (e.g. closure/simple.mmt)")
    args = parser.parse_args(argv)

    if not cargo.is_available():
        console.fail("The language suite runs through the marmot tool, which needs cargo.")
        return 1

    tree = BuildTree.from_args(args)
    selection = [*([args.category] if args.category else []),
                 *(["--pattern", args.pattern] if args.pattern else []),
                 *(["--test", args.test] if args.test else [])]
    # Debug builds are unoptimized and carry a type tag on every value: the
    # slowest test, dogfood/collections, takes about 45 s there against 1 s in Dev.
    timeout = ["--timeout-ms", "300000"] if tree.build_type == "Debug" else []
    with removing_new_files(REPO_ROOT):
        return cargo.run_tool(["test", *selection, *timeout], tree.require_compiler())


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
