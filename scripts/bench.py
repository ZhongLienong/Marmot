#!/usr/bin/env python3
"""
Marmot Benchmark Runner

Runs the benchmark suite and reports per-benchmark medians. Optionally
compares two executables with interleaved runs so machine drift affects
both sides equally.

Usage:
    python scripts/bench.py                          # current Release build
    python scripts/bench.py --runs 7                 # more samples
    python scripts/bench.py --compare path/to/old.exe
    python scripts/bench.py --exe path/to/marmot.exe
"""

import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WORKLOADS = [
    ROOT / "benchmarks" / "all.mmt",
    ROOT / "benchmarks" / "perf_sort_100k.mmt",
    ROOT / "benchmarks" / "perf_text_midsize.mmt",
    ROOT / "benchmarks" / "gc_churn.mmt",
]

RESULT_PATTERN = re.compile(r"^(.*?)(?: benchmark)? took (\d+) milliseconds", re.MULTILINE)


def find_executable() -> Path:
    candidates = [
        ROOT / "out/build/ninja/x64-release/out/Marmot.exe",
        ROOT / "out/build/x64-release/out/Marmot.exe",
        ROOT / "build/out/Marmot.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    sys.exit("No Release Marmot.exe found; pass --exe explicitly.")


def run_workload(exe: Path, workload: Path) -> dict[str, int]:
    proc = subprocess.run(
        [str(exe), "run", str(workload)],
        capture_output=True,
        text=True,
        cwd=ROOT,
        timeout=600,
    )
    results = {}
    for match in RESULT_PATTERN.finditer(proc.stdout):
        label = re.sub(r"\x1b\[[0-9;]*m", "", match.group(1)).strip()
        label = label.split(":")[0].strip()
        results[label] = int(match.group(2))
    if not results:
        sys.exit(f"{workload.name}: no benchmark output (exit={proc.returncode})\n{proc.stdout}\n{proc.stderr}")
    return results


def collect(exes: list[Path], runs: int) -> dict[str, dict[str, list[int]]]:
    samples: dict[str, dict[str, list[int]]] = {str(exe): {} for exe in exes}
    for run_index in range(runs):
        for workload in WORKLOADS:
            for exe in exes:  # interleave executables within each workload
                for label, ms in run_workload(exe, workload).items():
                    samples[str(exe)].setdefault(label, []).append(ms)
        print(f"  run {run_index + 1}/{runs} done", file=sys.stderr)
    return samples


def main() -> None:
    parser = argparse.ArgumentParser(description="Run Marmot benchmarks and report medians.")
    parser.add_argument("--exe", type=Path, default=None, help="Executable to benchmark")
    parser.add_argument("--compare", type=Path, default=None, help="Baseline executable for A/B comparison")
    parser.add_argument("--runs", type=int, default=5, help="Samples per benchmark (default 5)")
    args = parser.parse_args()

    exe = args.exe if args.exe else find_executable()
    exes = [exe]
    if args.compare:
        exes.insert(0, args.compare)

    samples = collect(exes, args.runs)

    labels: list[str] = []
    for per_exe in samples.values():
        for label in per_exe:
            if label not in labels:
                labels.append(label)

    name_width = max(len(label) for label in labels) + 2
    if args.compare:
        print(f"{'benchmark':<{name_width}}{'baseline':>10}{'current':>10}{'delta':>9}")
        for label in labels:
            base = statistics.median(samples[str(args.compare)].get(label, [0]))
            curr = statistics.median(samples[str(exe)].get(label, [0]))
            delta = f"{(curr - base) / base * 100:+.1f}%" if base else "n/a"
            print(f"{label:<{name_width}}{base:>8.0f}ms{curr:>8.0f}ms{delta:>9}")
    else:
        print(f"{'benchmark':<{name_width}}{'median':>10}{'min':>8}{'max':>8}")
        for label in labels:
            values = samples[str(exe)][label]
            print(f"{label:<{name_width}}{statistics.median(values):>8.0f}ms{min(values):>6}ms{max(values):>6}ms")


if __name__ == "__main__":
    main()
