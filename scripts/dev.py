#!/usr/bin/env python3
"""
One front door for everyday Marmot work, on Windows and Linux.

    python scripts/dev.py <command> [options]
    python scripts/dev.py <command> --help

Commands that need a build (run, test, snapshot, ...) bring it up to date
first; --no-build skips that. --build picks the configuration (Debug,
Development, Release, Experimental; Development unless the command says) and
--preset a CMake preset by name.
"""

from __future__ import annotations

import argparse
import importlib
import sys
from dataclasses import dataclass, field
from pathlib import Path

if sys.version_info < (3, 10):
    raise SystemExit("The Marmot scripts need Python 3.10 or newer.")

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib import console
from lib.presets import BuildTree

COMPILER_AND_VM = ["marmotc", "marmotvm"]


@dataclass(frozen=True)
class Command:
    module: str
    summary: str
    group: str
    targets: list[str] = field(default_factory=list)
    default_build: str | None = "Development"


COMMANDS: dict[str, Command] = {
    "doctor": Command("make.doctor", "check the toolchain and show what is built", "Build"),
    "configure": Command("make.configure", "(re)configure a CMake build tree", "Build"),
    "build": Command("make.compile", "build compiler, vm, unit, tool or all", "Build"),
    "clean": Command("make.clean", "delete a build tree, or --all build output", "Build"),
    "wasm": Command("make.wasm", "build for WebAssembly, and --deploy it to a website", "Build"),
    "run": Command("program.run", "build a .mmt file and run it", "Programs", COMPILER_AND_VM),
    "fmt": Command("program.fmt", "format .mmt files and folders, or --check them", "Programs", ["marmotc"]),
    "test": Command("testing.language", "run the language suite (--category, --pattern, --test)", "Tests", COMPILER_AND_VM),
    "unit": Command("testing.unit", "run the C++ unit tests (--tag, --regex)", "Tests", ["MarmotUnitTests"]),
    "snapshot": Command("testing.snapshot", "write a language test's .expected from its output", "Tests", COMPILER_AND_VM),
    "check": Command("", "run one check: layering, docs, cli, format, benchmarks, tool", "Tests"),
    "gate": Command("testing.gate", "everything a commit must pass", "Tests"),
    "bench": Command("bench.bench", "time the benchmarks, or --compare two compilers", "Performance", COMPILER_AND_VM, "Release"),
    "install": Command("install.install", "install marmotc, marmotvm, marmot and the prelude", "Install"),
    "uninstall": Command("install.uninstall", "remove that install", "Install"),
}

CHECKS: dict[str, Command] = {
    "layering": Command("testing.layering", "library include rules", "Tests"),
    "docs": Command("testing.doc_examples", "documentation examples (--sync to update their mirrors)", "Tests", COMPILER_AND_VM),
    "cli": Command("testing.cli_contracts", "command-line contracts", "Tests", COMPILER_AND_VM),
    "format": Command("testing.formatting", "formatter idempotency", "Tests", ["marmotc"]),
    "benchmarks": Command("testing.benchmarks", "benchmarks compile (--run to run them)", "Tests", COMPILER_AND_VM),
    "tool": Command("testing.tool", "the marmot tool's tests", "Tests", ["marmotc"]),
}


def usage() -> str:
    lines = [__doc__.strip(), ""]
    groups: dict[str, list[str]] = {}
    for name, command in COMMANDS.items():
        groups.setdefault(command.group, []).append(f"  {name:<11}{command.summary}")
    for group, entries in groups.items():
        lines += [f"{console.Color.BOLD}{group}{console.Color.RESET}", *entries, ""]
    lines.append(f"{console.Color.BOLD}Checks{console.Color.RESET} (dev.py check <name>)")
    lines += [f"  {name:<11}{check.summary}" for name, check in CHECKS.items()]
    return "\n".join(lines)


def bring_up_to_date(command: Command, argv: list[str]) -> tuple[int, list[str]]:
    """Build what the command needs, and take --no-build out of its arguments."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--build", default=command.default_build)
    parser.add_argument("--preset", default=None)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("-h", "--help", action="store_true")
    known, _ = parser.parse_known_args(argv)
    remaining = [argument for argument in argv if argument != "--no-build"]
    if not command.targets or known.no_build or known.help:
        return 0, remaining
    return BuildTree.select(known.build or "Development", known.preset).build(command.targets), remaining


def dispatch(command_name: str, command: Command, argv: list[str]) -> int:
    built, remaining = bring_up_to_date(command, argv)
    if built != 0:
        console.fail(f"the build failed, so `{command_name}` did not run")
        return built
    return importlib.import_module(command.module).main(remaining)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(usage())
        return 0

    name, rest = argv[0], argv[1:]
    if name not in COMMANDS:
        print(usage())
        console.fail(f"no command '{name}'")
        return 2
    if name != "check":
        return dispatch(name, COMMANDS[name], rest)

    if not rest or rest[0] not in CHECKS:
        console.fail(f"dev.py check needs one of: {', '.join(CHECKS)}")
        return 2
    return dispatch(f"check {rest[0]}", CHECKS[rest[0]], rest[1:])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
