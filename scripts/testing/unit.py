#!/usr/bin/env python3
"""
Run the C++ unit tests (the project test binaries, Catch2).

By default every test runs through CTest; --tag runs the Catch2 binary directly
with a tag expression, and --regex narrows CTest to matching names.

Examples:
    python scripts/dev.py unit
    python scripts/dev.py unit --tag "[runtime]"
    python scripts/dev.py unit --regex TypeChecker
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import toolchain
from lib.host import checkout_environment
from lib.presets import BuildTree, add_build_arguments


def test_names(binary: Path, tag: str = "") -> list[str]:
    listed = subprocess.run([str(binary), "--list-tests", "--reporter", "xml", *([tag] if tag else [])],
                            capture_output=True, text=True, check=True)
    return ["".join(name.itertext()) for name in ET.fromstring(listed.stdout).findall("TestCase/Name")]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the C++ unit tests.")
    add_build_arguments(parser)
    parser.add_argument("--tag", default="", help="A Catch2 tag expression, e.g. \"[runtime]\"; runs the project test binaries directly.")
    parser.add_argument("--regex", default="", help="A CTest name regex, e.g. TypeChecker.")
    args = parser.parse_args(argv)

    tree = BuildTree.from_args(args)
    if args.tag:
        if not any(test_names(binary, args.tag) for binary in (*tree.unit_tests, tree.integration_tests)):
            raise SystemExit(f"No test cases matched {args.tag!r}")
    for project, binary in zip(tree.projects, tree.unit_tests):
        if not binary.is_file():
            raise SystemExit(f"{binary} is not built. Build it with: python scripts/dev.py build unit --build {tree.build_type}")
        command = [str(binary), args.tag, "--allow-running-no-tests"] if args.tag else [
            "ctest", "--test-dir", str(project.binary_dir), "--output-on-failure",
            *(["-R", args.regex] if args.regex else [])]
        status = toolchain.run(command, environment=toolchain.build_environment())
        if status != 0:
            return status
    arguments = [str(tree.integration_tests), "--allow-running-no-tests"]
    if args.tag:
        arguments.append(args.tag)
    elif args.regex:
        names = [name for name in test_names(tree.integration_tests) if re.search(args.regex, name)]
        if not names:
            return 0
        arguments.extend(name.replace(",", "\\,") for name in names)
    return toolchain.run(arguments, environment=checkout_environment(MARMOTVM=str(tree.vm)))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
