#!/usr/bin/env python3
"""
Build a Marmot file with this checkout's compiler and run it in its VM.

Imports resolve against this checkout's prelude first.

Examples:
    python scripts/dev.py run examples/Hello.mmt
    python scripts/dev.py run --build Release benchmarks/all.mmt
    python scripts/dev.py run --check scratch.mmt
    python scripts/dev.py run -o scratch.mmc scratch.mmt
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import checkout_environment
from lib.presets import BuildTree, add_build_arguments


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build a Marmot file with this checkout's compiler and run it.")
    add_build_arguments(parser)
    parser.add_argument("--check", action="store_true", help="Only type-check the file.")
    parser.add_argument("-o", "--output", default=None, help="Keep the built .mmc here.")
    parser.add_argument("--embed-sources", action="store_true", help="Embed the sources, for runtime errors that show them.")
    parser.add_argument("file", help="The .mmt file to run.")
    args = parser.parse_args(argv)

    tree = BuildTree.from_args(args)
    compiler = str(tree.require_compiler())
    environment = checkout_environment()
    if args.check:
        return subprocess.run([compiler, "check", args.file], env=environment, check=False).returncode

    with tempfile.TemporaryDirectory(prefix="marmot-run-") as directory:
        program = args.output if args.output else str(Path(directory) / (Path(args.file).stem + ".mmc"))
        build = [compiler, "build", args.file, "-o", program, "--quiet", *(["--embed-sources"] if args.embed_sources else [])]
        built = subprocess.run(build, env=environment, check=False)
        if built.returncode != 0:
            return built.returncode
        return subprocess.run([str(tree.vm), program], env=environment, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
