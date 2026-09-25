#!/usr/bin/env python3
"""
Format Marmot files and folders with this checkout's formatter.

Rewrites the files in place; --check only reports the ones that would change.

Examples:
    python scripts/dev.py fmt MarmotPrelude
    python scripts/dev.py fmt test/closure/simple.mmt examples
    python scripts/dev.py fmt --check test MarmotPrelude
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import checkout_environment
from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Format Marmot files and folders.")
    add_build_arguments(parser)
    parser.add_argument("--check", action="store_true", help="Report the files that are not formatted; change nothing.")
    parser.add_argument("paths", nargs="+", help="Files and folders of .mmt files.")
    args = parser.parse_args(argv)

    compiler = str(BuildTree.from_args(args).require_compiler())
    mode = "--check" if args.check else "--write"
    return subprocess.run([compiler, "fmt", *args.paths, mode], env=checkout_environment(), check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
