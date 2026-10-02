#!/usr/bin/env python3
"""Measure compiler wall time, stages, and worker occupancy at different job counts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT as ROOT, checkout_environment
from lib.presets import BuildTree, add_build_arguments


def positive(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return parsed


def synthetic(root: Path, count: int) -> list[Path]:
    entries = []
    for shape in ("wide", "chain"):
        directory = root / shape
        directory.mkdir()
        for index in range(count):
            name = f"M{index:03d}"
            dependency = f"M{index - 1:03d}" if shape == "chain" and index else None
            text = f"module {name}\npublic export {{ Value }}\n"
            if dependency:
                text += f'import {{ "{dependency}.mmt" }}\n'
            text += "def F0 = fn(value: Int) -> Int => value + 1;\n"
            for function in range(1, 32):
                text += f"def F{function} = fn(value: Int) -> Int => F{function - 1}(value) + 1;\n"
            argument = f"{dependency}::Value(value)" if dependency else "value"
            text += f"def Value = fn(value: Int) -> Int => F31({argument});\n"
            (directory / f"{name}.mmt").write_text(text, encoding="utf-8", newline="\n")
        imports = list(range(count)) if shape == "wide" else [count - 1]
        text = 'module Main\nimport { ' + ", ".join(f'"M{i:03d}.mmt"' for i in imports) + " }\n"
        text += "def result = " + " + ".join(f"M{i:03d}::Value(0)" for i in imports) + ";\n"
        entry = directory / "Main.mmt"
        entry.write_text(text, encoding="utf-8", newline="\n")
        entries.append(entry)
    return entries


def sample(exe: Path, source: Path, artifact: Path, jobs: int | None, baseline: bool) -> tuple[dict, str]:
    command = [str(exe), "build", str(source), "-o", str(artifact), "--quiet"]
    if not baseline:
        command.append("--timings")
        if jobs is not None:
            command += ["--jobs", str(jobs)]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=ROOT, env=checkout_environment(), capture_output=True,
                               text=True, encoding="utf-8", errors="replace", timeout=600)
    elapsed = (time.perf_counter() - started) * 1000
    if completed.returncode:
        raise RuntimeError(f"{source}: compiler exited {completed.returncode}\n{completed.stdout}{completed.stderr}")
    measurements = {name: float(value) for name, value in re.findall(r"^  (.*): ([0-9.]+) ms$", completed.stderr, re.MULTILINE)}
    measurements["wall"] = elapsed
    if not baseline:
        workers = re.search(r"Workers: (\d+) configured, (\d+) peak active, (\d+) modules started", completed.stderr)
        occupancy = re.search(r"Worker occupancy: ([0-9.]+) average active, ([0-9.]+)%", completed.stderr)
        if workers is None or occupancy is None:
            raise RuntimeError(f"missing compiler timings:\n{completed.stderr}")
        measurements.update(workers=int(workers[1]), peak=int(workers[2]), modules_started=int(workers[3]),
                            average=float(occupancy[1]), occupancy=float(occupancy[2]))
    return measurements, hashlib.sha256(artifact.read_bytes()).hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_build_arguments(parser, default="Release")
    parser.add_argument("files", nargs="*", type=Path, help="Sources to compile; defaults to all.mmt and perf_sort_100k.mmt")
    parser.add_argument("--exe", type=Path, help="Compiler instead of this build's")
    parser.add_argument("--compare", type=Path, help="Earlier compiler, using its default scheduling without timing flags")
    parser.add_argument("--compare-jobs", action="store_true", help="Compare earlier compiler at the same job counts; requires its --jobs and --timings support")
    parser.add_argument("--jobs", nargs="+", type=positive, default=[1, 2, 4, 8], help="Worker limits to compare")
    parser.add_argument("--runs", type=positive, default=7)
    parser.add_argument("--synthetic", type=positive, help="Also generate wide and chain graphs with this many modules")
    parser.add_argument("--json", type=Path, help="Save measurements, including every sample")
    args = parser.parse_args(argv)
    exe = args.exe.resolve() if args.exe else BuildTree.from_args(args).require_compiler()
    baseline = args.compare.resolve() if args.compare else None
    sources = [path.resolve() for path in args.files] or [ROOT / "benchmarks/all.mmt", ROOT / "benchmarks/perf_sort_100k.mmt"]
    report = []
    with tempfile.TemporaryDirectory(prefix="marmot-compiler-bench-") as raw:
        directory = Path(raw)
        if args.synthetic:
            sources += synthetic(directory, args.synthetic)
        sides = [("current", exe, jobs) for jobs in [*dict.fromkeys(args.jobs), None]]
        if baseline:
            if args.compare_jobs:
                sides = [("baseline", baseline, jobs) for jobs in [*dict.fromkeys(args.jobs), None]] + sides
            else:
                sides.insert(0, ("baseline", baseline, None))
        for source in sources:
            label = f"{source.parent.name}/{source.name}"
            samples = {(side, jobs): [] for side, _, jobs in sides}
            expected = None
            for run in range(args.runs + 1):
                order = sides[run % len(sides):] + sides[:run % len(sides)]
                for side, compiler, jobs in order:
                    measured, digest = sample(compiler, source, directory / "program.mmc", jobs, side == "baseline" and not args.compare_jobs)
                    if expected is not None and digest != expected:
                        raise RuntimeError(f"{label}: emitted bytecode differs for {side}, jobs={jobs}")
                    expected = digest
                    if run:
                        samples[side, jobs].append(measured)
            print(f"\n{label}")
            print(f"{'side/jobs':<18}{'wall ms':>10}{'total ms':>11}{'discover':>11}{'modules':>11}{'avg active':>12}")
            for side, _, jobs in sides:
                values = samples[side, jobs]
                medians = {key: statistics.median(value[key] for value in values) for key in values[0]}
                row = {"source": label, "side": side, "jobs": jobs, "median": medians, "samples": values}
                report.append(row)
                shown = lambda key: f"{medians[key]:.3f}" if key in medians else "-"
                print(f"{side + '/' + (str(jobs) if jobs else 'auto'):<18}{shown('wall'):>10}{shown('total'):>11}"
                      f"{shown('discovery'):>11}{shown('modules'):>11}{shown('average'):>12}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
