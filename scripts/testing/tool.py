#!/usr/bin/env python3
"""
Run the Rust marmot tool's tests: its unit tests, and end-to-end runs against
this checkout's compiler (given to them as MARMOTC).

Example:
    python scripts/dev.py check tool
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import cargo, console
from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the marmot tool's tests against this checkout's compiler.")
    add_build_arguments(parser)
    parser.add_argument("--verbose", action="store_true", help="Accepted for consistency with the other checks.")
    args = parser.parse_args(argv)
    if not cargo.is_available():
        console.note("Skipping the marmot tool tests: cargo is not installed.")
        return 0
    return cargo.test(BuildTree.from_args(args).require_compiler())


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
