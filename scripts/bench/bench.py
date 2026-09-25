#!/usr/bin/env python3
"""
Run the benchmark suite and report per-benchmark medians.

Optionally compares two compilers with interleaved runs, so machine drift
affects both sides equally; each is run with the marmotvm beside it. To keep a
baseline, copy a build's out/ folder aside before changing the sources.

Usage:
    python scripts/dev.py bench                         # the Release build
    python scripts/dev.py bench --runs 7                # more samples
    python scripts/dev.py bench --compare old/out/marmotc
    python scripts/dev.py bench --exe path/to/marmotc
"""

from __future__ import annotations

import argparse
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT as ROOT
from lib.host import checkout_environment
from lib.presets import BuildTree, add_build_arguments
from lib.program import vm_beside

WORKLOADS = [
    ROOT / "benchmarks" / "all.mmt",
    ROOT / "benchmarks" / "perf_sort_100k.mmt",
    ROOT / "benchmarks" / "perf_text_midsize.mmt",
    ROOT / "benchmarks" / "gc_churn.mmt",
]

RESULT_PATTERN = re.compile(r"^(.*?)(?: benchmark)? took (\d+) milliseconds", re.MULTILINE)


def run_workload(exe: Path, workload: Path) -> dict[str, int]:
    # marmotc builds the workload; marmotvm (beside it) runs what is measured.
    environment = checkout_environment()
    with tempfile.TemporaryDirectory(prefix="marmot-bench-") as directory:
        program = Path(directory) / (workload.stem + ".mmc")
        subprocess.run([str(exe), "build", str(workload), "-o", str(program), "--quiet"],
                       cwd=ROOT, env=environment, timeout=600, check=True, capture_output=True)
        proc = subprocess.run(
            [str(vm_beside(exe)), str(program)],
            capture_output=True,
            text=True,
            cwd=ROOT,
            env=environment,
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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run Marmot benchmarks and report medians.")
    add_build_arguments(parser, default="Release")
    parser.add_argument("--exe", type=Path, default=None, help="marmotc to benchmark, instead of the build's")
    parser.add_argument("--compare", type=Path, default=None, help="Baseline marmotc for A/B comparison")
    parser.add_argument("--runs", type=int, default=5, help="Samples per benchmark (default 5)")
    args = parser.parse_args(argv)

    exe = args.exe.resolve() if args.exe else BuildTree.from_args(args).require_compiler()
    exes = [exe]
    if args.compare:
        exes.insert(0, args.compare.resolve())

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
            base = statistics.median(samples[str(exes[0])].get(label, [0]))
            curr = statistics.median(samples[str(exe)].get(label, [0]))
            delta = f"{(curr - base) / base * 100:+.1f}%" if base else "n/a"
            print(f"{label:<{name_width}}{base:>8.0f}ms{curr:>8.0f}ms{delta:>9}")
    else:
        print(f"{'benchmark':<{name_width}}{'median':>10}{'min':>8}{'max':>8}")
        for label in labels:
            values = samples[str(exe)][label]
            print(f"{label:<{name_width}}{statistics.median(values):>8.0f}ms{min(values):>6}ms{max(values):>6}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
