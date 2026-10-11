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
from lib.host import REPO_ROOT, TEST_DIR, checkout_environment, removing_new_files
from lib.presets import BuildTree, add_build_arguments
from lib.program import build_and_run
from lib.snapshots import snapshot_text, split_warnings


def locate(name: str) -> Path:
    candidates = [Path(name), REPO_ROOT / name, TEST_DIR / name]
    found = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
    if found is None:
        raise SystemExit(f"No test at {name} (looked beside you, in the checkout and under test/).")
    return found


def record(compiler: Path, timeout: float, test: Path, *, force: bool, warnings: bool) -> int:
    expected_file = test.with_suffix(".expected")
    warnings_file = test.with_suffix(".warnings.json")
    if expected_file.exists() and not force:
        console.fail(f"{expected_file.relative_to(REPO_ROOT).as_posix()} exists; pass --force to rewrite it.")
        return 1

    # Run from the checkout root, as the suite does, and clear away what the test leaves there.
    environment = checkout_environment(MARMOT_TEST_WARNING_FORMAT="machine")
    relative = test.relative_to(REPO_ROOT)
    with removing_new_files(REPO_ROOT):
        result = build_and_run(compiler, relative.as_posix(), cwd=REPO_ROOT, env=environment, timeout=timeout)
    recorded_warnings, human_output = split_warnings(result.stdout + result.stderr)

    expected_to_fail = any(part.lower() == "failure" for part in relative.parent.parts)
    if (result.returncode != 0) != expected_to_fail:
        console.note(f"Warning: exit code {result.returncode}, but a test {'under' if expected_to_fail else 'outside'} a failure/ folder "
                     f"is expected to {'fail' if expected_to_fail else 'succeed'}; the suite will fail it.")

    output = snapshot_text(human_output).strip()
    if output or expected_file.exists():
        expected_file.write_text(output + "\n", encoding="utf-8")
        console.ok(f"wrote {expected_file.relative_to(REPO_ROOT).as_posix()}")
    if warnings or warnings_file.exists():
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

    tree = BuildTree.from_args(args)
    # Debug builds are unoptimized and carry a type tag on every value: the
    # slowest test, dogfood/collections, takes about 45 s there against 1 s in Dev.
    timeout = 300.0 if tree.build_type == "Debug" else 30.0
    return max(record(tree.require_compiler(), timeout, locate(name), force=args.force, warnings=args.warnings) for name in args.tests)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
