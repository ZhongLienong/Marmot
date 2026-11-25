#!/usr/bin/env python3
"""Merge compile_commands.json from all three CMake projects into one.

Usage:
    python scripts/make/merge_compile_commands.py [--build Dev]

Writes the merged database to out/build/compile_commands.json so that
.clangd's CompilationDatabase can point at out/build/.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT, preset_family

PROJECTS = ("marmotc", "marmotvm", "marmotstdlib")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Merge compile_commands.json from all projects.")
    parser.add_argument("--build", default="Dev", choices=["Debug", "Dev", "Release"],
                        help="Build type to merge (default: Dev)")
    args = parser.parse_args(argv)

    preset = preset_family() + args.build.lower()
    merged: list[dict] = []
    missing: list[str] = []

    for project in PROJECTS:
        path = REPO_ROOT / "out" / "build" / project / preset / "compile_commands.json"
        if path.is_file():
            merged.extend(json.loads(path.read_text(encoding="utf-8")))
        else:
            missing.append(str(path))

    if missing:
        print(f"Warning: missing compile databases (build first?):", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)

    if not merged:
        print("Error: no compile commands found. Run `python scripts/dev.py build` first.", file=sys.stderr)
        return 1

    output = REPO_ROOT / "out" / "build" / "compile_commands.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(merged)} entries to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
