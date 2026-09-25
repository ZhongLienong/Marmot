#!/usr/bin/env python3
"""
The full gate: everything that must pass before a commit.

In order, stopping at the first failure:
    layering    the compiler, runtime and common libraries include only what they may
    build       marmotc, marmotvm and MarmotUnitTests
    unit        the C++ unit tests
    docs        documentation examples are in sync and run as documented
    cli         the command-line contracts
    format      the formatter is idempotent on the corpus
    benchmarks  every program under benchmarks/ compiles cleanly
    tool        the Rust marmot tool's tests, against the compiler just built
    language    the language suite under test/

Examples:
    python scripts/dev.py gate
    python scripts/dev.py gate --build Release
    python scripts/dev.py gate --skip unit tool
    python scripts/dev.py gate --only docs cli
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import console
from lib.host import REPO_ROOT
from lib.presets import BuildTree, add_build_arguments
from testing import benchmarks, cli_contracts, doc_examples, formatting, language, layering, tool, unit

STEPS = ["layering", "build", "unit", "docs", "cli", "format", "benchmarks", "tool", "language"]


def cleanup_all_artifacts(root: Path) -> None:
    patterns = (
        "*.ppm",
        "*.mmc",
        "*.mmc.json",
        ".doc_example_*.mmt",
        "midori_phase3_io_*",
        "midori_phase8_doc_*",
    )
    for pattern in patterns:
        for path in root.glob(pattern):
            if path.is_file():
                try:
                    path.unlink()
                except OSError:
                    pass
            elif path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
        for path in (root / "benchmarks").glob(pattern):
            if path.is_file():
                try:
                    path.unlink()
                except OSError:
                    pass
    for dir_path in root.glob("midori_phase3_missing_dir_*"):
        if dir_path.is_dir():
            shutil.rmtree(dir_path, ignore_errors=True)
    doc_examples_dir = root / ".doc_examples"
    if doc_examples_dir.is_dir():
        shutil.rmtree(doc_examples_dir, ignore_errors=True)


def tree_arguments(args: argparse.Namespace) -> list[str]:
    return ["--build", args.build, *(["--preset", args.preset] if args.preset else [])]


def build_everything(tree: BuildTree) -> int:
    if tree.build_type == "Release":
        # Release leaves the unit tests out unless configured with them.
        configured = tree.configure(with_unit_tests=True)
        if configured != 0:
            return configured
    return tree.build(["marmotc", "marmotvm", "MarmotUnitTests"])


def steps(args: argparse.Namespace, tree: BuildTree) -> dict[str, Callable[[], int]]:
    common = tree_arguments(args)
    verbose = ["--verbose"] if args.verbose else []
    return {
        "layering": lambda: layering.main([]),
        "build": lambda: build_everything(tree),
        "unit": lambda: unit.main(common),
        "docs": lambda: doc_examples.main(common + verbose),
        "cli": lambda: cli_contracts.main(common + verbose),
        "format": lambda: formatting.main(common + verbose),
        "benchmarks": lambda: benchmarks.main(common + verbose),
        "tool": lambda: tool.main(common),
        "language": lambda: language.main(common + verbose),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run every check a commit must pass.")
    add_build_arguments(parser)
    parser.add_argument("--skip", nargs="+", choices=STEPS, default=[], metavar="STEP", help=f"Steps to leave out: {', '.join(STEPS)}.")
    parser.add_argument("--only", nargs="+", choices=STEPS, default=[], metavar="STEP", help="Run just these steps.")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show each failure's output.")
    args = parser.parse_args(argv)

    tree = BuildTree.from_args(args)
    chosen = [step for step in STEPS if (not args.only or step in args.only) and step not in args.skip]
    runners = steps(args, tree)
    timings: list[tuple[str, float]] = []
    try:
        for step in chosen:
            print(f"\n{console.Color.BOLD}{console.Color.BLUE}== {step} =={console.Color.RESET}", flush=True)
            start = time.perf_counter()
            exit_code = runners[step]()
            timings.append((step, time.perf_counter() - start))
            if exit_code != 0:
                console.fail(f"gate stopped at '{step}' (exit {exit_code})")
                return exit_code

        print()
        for step, seconds in timings:
            console.ok(f"{step:<12}{seconds:7.1f}s")
        console.ok(f"gate passed on {tree.preset}")
        return 0
    finally:
        cleanup_all_artifacts(REPO_ROOT)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
