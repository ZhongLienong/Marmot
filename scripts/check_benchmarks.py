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
    python scripts/check_benchmarks.py --build Development
    python scripts/check_benchmarks.py --build Release --run
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

from run_tests import TestRunner, build_and_run


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def discover_benchmarks(root: Path) -> list[Path]:
    return sorted((root / "benchmarks").glob("*.mmt"))


def build_environment(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    separator = ";" if os.name == "nt" else ":"
    prelude_path = str((root / "MarmotPrelude").resolve())
    existing = env.get("MARMOT_PATH", "")
    env["MARMOT_PATH"] = prelude_path if existing == "" else separator.join([prelude_path, existing])
    return env


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
    try:
        completed = build_and_run(midori_exe, relative_path, cwd=root, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"did not finish within {timeout:.0f}s"

    elapsed = time.perf_counter() - start
    for line in (completed.stdout + completed.stderr).splitlines():
        print(f"    {line}")
    print(f"    ({elapsed:.2f}s wall, exit {completed.returncode})")
    if completed.returncode != 0:
        return f"exited with {completed.returncode}"
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Compile (and optionally run) every program under benchmarks/.")
    parser.add_argument(
        "--build",
        default="Development",
        choices=["Debug", "Development", "Release"],
        help="Build configuration used to locate marmotc.exe (default: Development).",
    )
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

    runner = TestRunner(build_config=args.build, verbose=args.verbose)
    print(f"Executable: {runner.midori_exe}")
    print(f"Build: {runner.build_config}")
    if runner.executable_notice:
        print(f"Notice: {runner.executable_notice}")

    env = build_environment(root)
    failures = 0
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


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
