#!/usr/bin/env python3
"""
Compile every program under benchmarks/ so neither can silently rot.

Neither directory is part of the regression suite: the benchmarks print timings
and some write image files, so none has a stable snapshot.
Nothing used to compile them, and on the v2 branch every one of them stopped
compiling unnoticed when the language dropped `loop`, assignment and in-place
`Appendable`. This check runs `marmot check` (the full compile pipeline, without
executing) on each one and fails on any compile error or warning.

Examples:
    python scripts/dev.py check benchmarks
    python scripts/dev.py check benchmarks --build Release --run
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT, checkout_environment
from lib.presets import BuildTree, add_build_arguments
from lib.program import build_and_run
from testing.language import TestRunner


def cleanup_benchmark_artifacts(root: Path, extra_files: set[Path] | None = None) -> None:
    patterns = ("*.ppm", "*.mmc", "*.mmc.json")
    for pattern in patterns:
        for path in root.glob(pattern):
            if path.is_file():
                try:
                    path.unlink()
                except OSError:
                    pass
        for path in (root / "benchmarks").glob(pattern):
            if path.is_file():
                try:
                    path.unlink()
                except OSError:
                    pass
    if extra_files:
        for path in extra_files:
            try:
                if path.is_file():
                    path.unlink()
                elif path.is_dir():
                    shutil.rmtree(path, ignore_errors=True)
            except OSError:
                pass


def repo_root() -> Path:
    return REPO_ROOT


def discover_benchmarks(root: Path) -> list[Path]:
    return sorted((root / "benchmarks").glob("*.mmt"))


def check_benchmark(root: Path, midori_exe: Path, benchmark: Path, env: dict[str, str]) -> Optional[str]:
    relative_path = str(benchmark.relative_to(root))
    completed = subprocess.run(
        [str(midori_exe), "check", relative_path, "--format", "json"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
        cwd=root,
        env=env,
        check=False,
    )

    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return f"could not parse `marmot check` output (exit {completed.returncode}):\n{completed.stdout}{completed.stderr}"

    report = payload.get("report", {})
    errors = report.get("errors", [])
    warnings = report.get("warnings", [])
    if completed.returncode != 0 or errors:
        messages = "\n".join(f"    {error.get('message', error)}" for error in errors)
        return f"does not compile (exit {completed.returncode}):\n{messages}"
    if warnings:
        messages = "\n".join(f"    {warning.get('message', warning)}" for warning in warnings)
        return f"compiles with {len(warnings)} warning(s):\n{messages}"
    return None


def run_benchmark(root: Path, midori_exe: Path, benchmark: Path, env: dict[str, str], timeout: float) -> Optional[str]:
    relative_path = str(benchmark.relative_to(root))
    start = time.perf_counter()
    files_before = {p for p in root.iterdir() if p.is_file()}
    try:
        completed = build_and_run(midori_exe, relative_path, cwd=root, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"did not finish within {timeout:.0f}s"
    finally:
        files_after = {p for p in root.iterdir() if p.is_file()}
        cleanup_benchmark_artifacts(root, extra_files=files_after - files_before)

    elapsed = time.perf_counter() - start
    for line in (completed.stdout + completed.stderr).splitlines():
        print(f"    {line}")
    print(f"    ({elapsed:.2f}s wall, exit {completed.returncode})")
    if completed.returncode != 0:
        return f"exited with {completed.returncode}"
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Compile (and optionally run) every program under benchmarks/.")
    add_build_arguments(parser)
    parser.add_argument(
        "--run",
        action="store_true",
        help="Also execute each program and print its output. Timings are only meaningful with --build Release.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="Per-benchmark run timeout in seconds when --run is given (default: 300).",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Accepted for consistency with the other check scripts.",
    )
    args = parser.parse_args(argv)

    root = repo_root()
    benchmarks = discover_benchmarks(root)
    if len(benchmarks) == 0:
        print("[FAIL] No programs found under benchmarks/.")
        return 1

    runner = TestRunner(BuildTree.from_args(args), verbose=args.verbose)
    print(f"Executable: {runner.midori_exe}")
    print(f"Build: {runner.build_config}")

    env = checkout_environment()
    failures = 0
    try:
        for benchmark in benchmarks:
            name = benchmark.relative_to(root).as_posix()
            failure = check_benchmark(root, runner.midori_exe, benchmark, env)
            if failure is None and args.run:
                print(f"[RUN] {name}")
                failure = run_benchmark(root, runner.midori_exe, benchmark, env, args.timeout)
            if failure is None:
                print(f"[OK] {name}")
            else:
                failures += 1
                print(f"[FAIL] {name} {failure}")

        if failures:
            print(f"\n[FAILED] {failures} of {len(benchmarks)} program(s) failed.")
            return 1

        action = "compiled and ran" if args.run else "compiled cleanly"
        print(f"\n[SUCCESS] {len(benchmarks)} program(s) {action}.")
        return 0
    finally:
        cleanup_benchmark_artifacts(root)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
