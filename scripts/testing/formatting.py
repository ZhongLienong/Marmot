#!/usr/bin/env python3
"""
Format-idempotency check for the Marmot source tree.

For every `.mmt` file under the configured roots, runs `marmotc fmt <file>`
twice and verifies that:

- the formatter succeeds on the original source
- the formatter produces identical output on the formatted source
  (`fmt(fmt(x)) == fmt(x)`)

Exits with a non-zero status when any file fails either check. Optionally also
runs `marmotc fmt <root> --check` to enforce that the corpus is already
formatted - controlled by `--enforce-clean`.

Usage:
    python scripts/dev.py check format
    python scripts/dev.py check format --build Debug
    python scripts/dev.py check format --root test --root MarmotPrelude
    python scripts/dev.py check format --enforce-clean
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT
from lib.presets import BuildTree, add_build_arguments
from testing.language import TestRunner


DEFAULT_ROOTS: tuple[str, ...] = (
    "test",
    "MarmotPrelude",
)


def repo_root() -> Path:
    return REPO_ROOT


def collect_mdr_files(roots: Sequence[Path]) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        if root.is_file() and root.suffix == ".mmt":
            files.append(root)
            continue
        for path in root.rglob("*.mmt"):
            if path.is_file():
                files.append(path)
    return sorted(set(files))


def check_idempotency(runner: TestRunner, files: Sequence[Path], verbose: bool) -> int:
    failures: list[tuple[Path, str]] = []
    for path in files:
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as exc:
            failures.append((path, f"could not read as UTF-8: {exc}"))
            continue

        completed_first = subprocess.run(
            [str(runner.midori_exe), "fmt", str(path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            cwd=repo_root(),
            check=False,
        )
        if completed_first.returncode != 0:
            # A fixture that must fail to lex cannot be formatted: the formatter works on tokens.
            if "failure" in path.parts and "Lexer Error" in completed_first.stdout + completed_first.stderr:
                continue
            failures.append((path, f"first format failed: {completed_first.stderr.strip()}"))
            continue

        formatted_once = completed_first.stdout

        sidecar = path.with_suffix(path.suffix + ".fmtcheck.tmp")
        sidecar.write_text(formatted_once, encoding="utf-8")
        try:
            completed_second = subprocess.run(
                [str(runner.midori_exe), "fmt", str(sidecar)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                cwd=repo_root(),
                check=False,
            )
        finally:
            try:
                sidecar.unlink()
            except OSError:
                pass

        if completed_second.returncode != 0:
            failures.append((path, f"second format failed: {completed_second.stderr.strip()}"))
            continue

        if completed_second.stdout != formatted_once:
            failures.append((path, "fmt(fmt(x)) != fmt(x)"))
            continue

        if verbose:
            print(f"[OK] {path.relative_to(repo_root())}")

    if failures:
        print(f"\n[FAILED] {len(failures)} idempotency check(s) failed.")
        for path, message in failures:
            print(f"- {path.relative_to(repo_root())}: {message}")
        return 1

    print(f"\n[SUCCESS] {len(files)} file(s) are formatter-idempotent.")
    return 0


def enforce_clean(runner: TestRunner, roots: Sequence[Path]) -> int:
    failures: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        completed = subprocess.run(
            [str(runner.midori_exe), "fmt", str(root), "--check"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
            cwd=repo_root(),
            check=False,
        )
        if completed.returncode != 0:
            failures.append(root)

    if failures:
        roots_text = ", ".join(str(path) for path in failures)
        print(f"\n[FAILED] formatter --check reported drift in: {roots_text}")
        return 1

    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run formatter idempotency checks.")
    add_build_arguments(parser)
    parser.add_argument(
        "--root",
        action="append",
        default=None,
        help="Source tree root to scan for .mmt files. May be repeated. Defaults to the canonical roots.",
    )
    parser.add_argument(
        "--enforce-clean",
        action="store_true",
        help="Also require `fmt --check` to pass on each root, blocking drift in committed files.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print one line per checked file.",
    )
    args = parser.parse_args(argv)

    runner = TestRunner(BuildTree.from_args(args), verbose=args.verbose)
    print(f"Executable: {runner.midori_exe}")
    print(f"Build: {runner.build_config}")

    raw_roots = args.root if args.root else list(DEFAULT_ROOTS)
    roots: list[Path] = []
    for raw_root in raw_roots:
        candidate = Path(raw_root)
        if not candidate.is_absolute():
            candidate = repo_root() / candidate
        roots.append(candidate)

    files = collect_mdr_files(roots)
    if not files:
        print("No .mmt files found under the requested roots.")
        return 0

    idempotency_exit_code = check_idempotency(runner, files, args.verbose)
    if idempotency_exit_code != 0:
        return idempotency_exit_code

    if args.enforce_clean:
        clean_exit_code = enforce_clean(runner, roots)
        if clean_exit_code != 0:
            return clean_exit_code

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
