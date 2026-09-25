#!/usr/bin/env python3
"""
Build Marmot: the compiler, the VM, the unit tests and the project tool.

Targets:
    compiler    marmotc
    vm          marmotvm
    unit        MarmotUnitTests
    tool        the Rust marmot tool, with cargo
    all         everything above

A build tree that does not exist yet is configured first.

Examples:
    python scripts/dev.py build
    python scripts/dev.py build all --build Release
    python scripts/dev.py build unit --build Debug
    python scripts/dev.py build tool --debug-tool
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import cargo
from lib.presets import BuildTree, add_build_arguments

CMAKE_TARGETS = {"compiler": "marmotc", "vm": "marmotvm", "unit": "MarmotUnitTests"}
TARGETS = [*CMAKE_TARGETS, "tool", "all"]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build the compiler, VM, unit tests or project tool.")
    add_build_arguments(parser)
    parser.add_argument("targets", nargs="*", metavar="TARGET",
                        help=f"What to build: {', '.join(TARGETS)} (default: compiler vm).")
    parser.add_argument("--debug-tool", action="store_true", help="Build the marmot tool without --release.")
    args = parser.parse_args(argv)

    unknown = [target for target in args.targets if target not in TARGETS]
    if unknown:
        parser.error(f"unknown target {', '.join(unknown)}; choose from {', '.join(TARGETS)}")
    requested = args.targets or ["compiler", "vm"]
    chosen = [*CMAKE_TARGETS, "tool"] if "all" in requested else requested
    cmake_targets = [CMAKE_TARGETS[target] for target in chosen if target in CMAKE_TARGETS]
    if cmake_targets:
        tree = BuildTree.from_args(args)
        if "MarmotUnitTests" in cmake_targets and tree.build_type == "Release":
            # Release leaves the unit tests out unless configured with them.
            configured = tree.configure(with_unit_tests=True)
            if configured != 0:
                return configured
        built = tree.build(cmake_targets)
        if built != 0:
            return built

    if "tool" in chosen:
        return cargo.build(release=not args.debug_tool)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
