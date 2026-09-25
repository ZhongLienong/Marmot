#!/usr/bin/env python3
"""
Write a language test's snapshot from what it prints now.

A test's .expected is its normalized output (colour stripped, checkout paths
made relative), and its .warnings.json, when there is one, its warnings. This
records them for a new test, or rewrites them after an intended change. Read
the diff before committing it: a snapshot rewritten to match a regression hides
the regression.

Examples:
    python scripts/dev.py snapshot closure/simple.mmt
    python scripts/dev.py snapshot test/closure/simple.mmt --force
    python scripts/dev.py snapshot static_analyzer/success/unused_local_warning.mmt --warnings
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import console
from lib.host import REPO_ROOT, TEST_DIR
from lib.presets import BuildTree, add_build_arguments
from testing.language import TestRunner


def locate(name: str) -> Path:
    candidates = [Path(name), REPO_ROOT / name, TEST_DIR / name]
    found = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
    if found is None:
        raise SystemExit(f"No test at {name} (looked beside you, in the checkout and under test/).")
    return found


def record(runner: TestRunner, test: Path, *, force: bool, warnings: bool) -> int:
    expected_file = test.with_suffix(".expected")
    warnings_file = test.with_suffix(".warnings.json")
    if expected_file.exists() and not force:
        console.fail(f"{expected_file.relative_to(REPO_ROOT).as_posix()} exists; pass --force to rewrite it.")
        return 1

    machine_warnings = warnings or warnings_file.exists()
    result = runner.execute(test, machine_warnings=machine_warnings)
    recorded_warnings, human_output = runner.split_machine_readable_warnings(result.stdout + result.stderr)

    expected_to_fail = runner.is_failure_test(test)
    if (result.returncode != 0) != expected_to_fail:
        console.note(f"Warning: exit code {result.returncode}, but a test {'under' if expected_to_fail else 'outside'} a failure/ folder "
                     f"is expected to {'fail' if expected_to_fail else 'succeed'}; the suite will fail it.")

    output = runner.normalize_snapshot_text(human_output).strip()
    if output or expected_file.exists():
        expected_file.write_text(output + "\n", encoding="utf-8")
        console.ok(f"wrote {expected_file.relative_to(REPO_ROOT).as_posix()}")
    if machine_warnings:
        warnings_file.write_text(json.dumps(recorded_warnings, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        console.ok(f"wrote {warnings_file.relative_to(REPO_ROOT).as_posix()} ({len(recorded_warnings)} warning(s))")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Write language tests' snapshots from their current output.")
    add_build_arguments(parser)
    parser.add_argument("tests", nargs="+", help="Test files: a path, or one relative to test/.")
    parser.add_argument("--force", action="store_true", help="Rewrite a snapshot that already exists.")
    parser.add_argument("--warnings", action="store_true", help="Also record a .warnings.json (kept up to date when one exists).")
    args = parser.parse_args(argv)

    runner = TestRunner(BuildTree.from_args(args))
    return max(record(runner, locate(name), force=args.force, warnings=args.warnings) for name in args.tests)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
