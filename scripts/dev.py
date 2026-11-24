#!/usr/bin/env python3
"""Build Marmot and run its CI gate on Windows and Linux."""

from __future__ import annotations

import importlib
import sys

if sys.version_info < (3, 10):
    raise SystemExit("The Marmot scripts need Python 3.10 or newer.")

COMMANDS = {
    "configure": ("make.configure", "(re)configure a CMake build tree"),
    "build": ("make.compile", "build the compiler, VM, standard library, tests, or tool"),
    "wasm": ("make.wasm", "build for WebAssembly"),
    "generate-mmc": ("make.generate_mmc", "generate or check the private .mmc headers and tables"),
    "gate": ("testing.gate", "run the CI gate"),
}


def usage() -> str:
    commands = "\n".join(f"  {name:<15}{summary}" for name, (_, summary) in COMMANDS.items())
    return (
        f"{__doc__}\n\n"
        "Usage: python scripts/dev.py <command> [options]\n\n"
        f"{commands}\n\n"
        "Use marmot run, marmot fmt, and marmot test to work with Marmot programs."
    )


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(usage())
        return 0

    name, *arguments = argv
    if name not in COMMANDS:
        print(usage(), file=sys.stderr)
        print(f"Unknown command '{name}'.", file=sys.stderr)
        return 2

    module, _ = COMMANDS[name]
    return importlib.import_module(module).main(arguments)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
