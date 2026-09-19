#!/usr/bin/env python3
"""
Marmot project test runner.

Wraps the existing unit-test and regression-test entry points so contributors can
configure, build, and run the test layers from one command.

Examples:
    python scripts/test_project.py
    python scripts/test_project.py --mode unit --build Debug
    python scripts/test_project.py --mode unit --unit-tag "[runtime]"
    python scripts/test_project.py --mode regression --category closure
    python scripts/test_project.py --mode all --build Release
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


def is_windows() -> bool:
    return os.name == "nt"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def load_cmake_presets(root: Path) -> dict[str, dict[str, Any]]:
    presets_path = root / "CMakePresets.json"
    contents = json.loads(presets_path.read_text(encoding="utf-8-sig"))
    configure_presets = contents.get("configurePresets", [])
    return {
        str(preset["name"]): preset
        for preset in configure_presets
        if isinstance(preset, dict) and "name" in preset
    }


def resolve_preset_definition(
    preset_name: str,
    presets: dict[str, dict[str, Any]],
    visited: set[str] | None = None,
) -> dict[str, Any]:
    if preset_name not in presets:
        available = ", ".join(sorted(presets))
        raise RuntimeError(f"Unknown CMake configure preset '{preset_name}'. Available presets: {available}")

    if visited is None:
        visited = set()

    if preset_name in visited:
        raise RuntimeError(f"Cyclic CMake preset inheritance detected at '{preset_name}'.")

    visited.add(preset_name)
    preset = presets[preset_name]
    resolved: dict[str, Any] = {}

    inherited = preset.get("inherits", [])
    if isinstance(inherited, str):
        inherited_names = [inherited]
    elif isinstance(inherited, list):
        inherited_names = [str(name) for name in inherited]
    else:
        inherited_names = []

    for inherited_name in inherited_names:
        resolved.update(resolve_preset_definition(inherited_name, presets, visited.copy()))

    resolved.update(preset)
    return resolved


def expand_preset_path(template: str, preset_name: str, root: Path) -> Path:
    expanded = template.replace("${sourceDir}", str(root)).replace("${presetName}", preset_name)
    return Path(expanded).resolve()


def resolve_binary_dir(preset_name: str, presets: dict[str, dict[str, Any]], root: Path) -> Path:
    resolved_preset = resolve_preset_definition(preset_name, presets)
    binary_dir = resolved_preset.get("binaryDir")
    if not isinstance(binary_dir, str) or binary_dir.strip() == "":
        raise RuntimeError(f"Preset '{preset_name}' does not define a binaryDir.")
    return expand_preset_path(binary_dir, preset_name, root)


def resolve_build_type(preset_name: str, presets: dict[str, dict[str, Any]], fallback_build: str) -> str:
    resolved_preset = resolve_preset_definition(preset_name, presets)
    cache_variables = resolved_preset.get("cacheVariables", {})
    if isinstance(cache_variables, dict):
        build_type = cache_variables.get("CMAKE_BUILD_TYPE")
        if isinstance(build_type, str) and build_type.strip() != "":
            return build_type

    return fallback_build


def default_preset_name(build_config: str, presets: dict[str, dict[str, Any]]) -> str:
    candidate = ("x64-" if is_windows() else "linux-") + build_config.lower()
    if candidate in presets:
        return candidate

    available = ", ".join(sorted(presets))
    raise RuntimeError(
        f"Could not infer a preset for build '{build_config}'. "
        f"Pass --preset explicitly. Available presets: {available}"
    )


def format_command(command: list[str]) -> str:
    if is_windows():
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def run_command(command: list[str], root: Path) -> int:
    print(f"\n> {format_command(command)}")
    completed = subprocess.run(command, cwd=root, check=False)
    return completed.returncode


def build_targets_for_mode(mode: str) -> list[str]:
    if mode == "unit":
        return ["MarmotUnitTests"]
    if mode == "regression":
        return ["marmotc"]
    return ["marmotc", "MarmotUnitTests"]


def unit_test_executable(binary_dir: Path) -> Path:
    executable_name = "MarmotUnitTests.exe" if is_windows() else "MarmotUnitTests"
    return binary_dir / "out" / executable_name


def compiler_executable(binary_dir: Path) -> Path:
    executable_name = "marmotc.exe" if is_windows() else "marmotc"
    return binary_dir / "out" / executable_name


def run_tool_tests(root: Path, binary_dir: Path) -> int:
    # The Rust `marmot` tool: its unit tests, plus end-to-end runs against the
    # compiler just built.
    if shutil.which("cargo") is None:
        print("\nSkipping the marmot tool tests: cargo is not installed.")
        return 0

    command = ["cargo", "test", "--manifest-path", str(root / "tool" / "Cargo.toml")]
    print(f"\n> MARMOTC={compiler_executable(binary_dir)} {format_command(command)}")
    completed = subprocess.run(command, cwd=root, check=False, env={**os.environ, "MARMOTC": str(compiler_executable(binary_dir))})
    return completed.returncode


def build_config_for_regressions(build_config: str) -> str:
    supported = {"Debug", "Development", "Release"}
    if build_config in supported:
        return build_config

    supported_list = ", ".join(sorted(supported))
    raise RuntimeError(
        f"Regression tests do not support build '{build_config}'. "
        f"Use one of: {supported_list}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Configure, build, and run Marmot unit tests, regression tests, or both."
    )
    parser.add_argument(
        "--mode",
        choices=["all", "unit", "regression"],
        default="all",
        help="Which test layers to run (default: all).",
    )
    parser.add_argument(
        "--build",
        choices=["Debug", "Development", "Release", "Experimental"],
        default="Development",
        help="Build configuration used to infer the default preset (default: Development).",
    )
    parser.add_argument(
        "--preset",
        default="",
        help="Explicit CMake configure/build preset. Overrides --build preset inference.",
    )
    parser.add_argument(
        "--skip-configure",
        action="store_true",
        help="Skip the configure step and reuse the existing build tree.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip the build step and reuse the existing binaries.",
    )
    parser.add_argument(
        "--unit-regex",
        default="",
        help="CTest name regex for unit tests, for example 'TypeChecker' or 'ImportResolver'.",
    )
    parser.add_argument(
        "--unit-tag",
        default="",
        help="Catch2 tag expression for unit tests, for example '[runtime]'. Runs MarmotUnitTests directly.",
    )
    parser.add_argument(
        "--category",
        default="",
        help="Regression-test category passed through to scripts/run_tests.py.",
    )
    parser.add_argument(
        "--pattern",
        default="",
        help="Regression-test filename pattern passed through to scripts/run_tests.py.",
    )
    parser.add_argument(
        "--test",
        default="",
        help="Specific regression test path/name passed through to scripts/run_tests.py.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose regression-test output.",
    )
    parser.add_argument(
        "--skip-doc-examples",
        action="store_true",
        help="Skip the doc-example sync/compile check that normally runs with full regression passes.",
    )
    parser.add_argument(
        "--skip-cli-contracts",
        action="store_true",
        help="Skip the CLI contract check that normally runs with full regression passes.",
    )
    parser.add_argument(
        "--skip-format-check",
        action="store_true",
        help="Skip the formatter idempotency check that normally runs with full regression passes.",
    )
    parser.add_argument(
        "--skip-benchmark-check",
        action="store_true",
        help="Skip compiling the programs under benchmarks/, which normally runs with full regression passes.",
    )
    parser.add_argument(
        "--skip-tool-tests",
        action="store_true",
        help="Skip the Rust marmot tool's tests, which normally run with full regression passes.",
    )
    parser.add_argument(
        "--skip-layering-check",
        action="store_true",
        help="Skip the include check that keeps the compiler, runtime and common libraries apart.",
    )
    args = parser.parse_args(argv)

    root = repo_root()

    # Needs no build, so it runs first and fails fast.
    if args.skip_layering_check is False:
        layering_exit_code = run_command([sys.executable, str(root / "scripts" / "check_layering.py")], root)
        if layering_exit_code != 0:
            return layering_exit_code
    presets = load_cmake_presets(root)
    preset_name = args.preset if args.preset else default_preset_name(args.build, presets)
    binary_dir = resolve_binary_dir(preset_name, presets, root)
    effective_build = resolve_build_type(preset_name, presets, args.build)

    needs_unit = args.mode in {"all", "unit"}
    needs_regression = args.mode in {"all", "regression"}

    if args.skip_configure is False:
        configure_command = ["cmake", "--preset", preset_name]
        if needs_unit and effective_build == "Release":
            configure_command.append("-DMIDORI_BUILD_TESTS=ON")

        configure_exit_code = run_command(configure_command, root)
        if configure_exit_code != 0:
            return configure_exit_code

    if args.skip_build is False:
        build_command = ["cmake", "--build", "--preset", preset_name, "--target", *build_targets_for_mode(args.mode)]
        build_exit_code = run_command(build_command, root)
        if build_exit_code != 0:
            return build_exit_code

    if needs_unit:
        if args.unit_tag:
            unit_executable = unit_test_executable(binary_dir)
            if unit_executable.exists() is False:
                raise RuntimeError(f"Unit test executable not found: {unit_executable}")
            unit_command = [str(unit_executable), args.unit_tag]
        else:
            if binary_dir.exists() is False:
                raise RuntimeError(f"CTest build directory not found: {binary_dir}")
            unit_command = ["ctest", "--test-dir", str(binary_dir), "--output-on-failure"]
            if args.unit_regex:
                unit_command.extend(["-R", args.unit_regex])

        unit_exit_code = run_command(unit_command, root)
        if unit_exit_code != 0:
            return unit_exit_code

    if needs_regression:
        regression_build = build_config_for_regressions(effective_build)
        should_run_doc_examples = not args.skip_doc_examples
        should_run_cli_contracts = not args.skip_cli_contracts
        should_run_format_check = not args.skip_format_check
        should_run_benchmark_check = not args.skip_benchmark_check
        should_run_tool_tests = not args.skip_tool_tests
        if args.category or args.pattern or args.test:
            should_run_doc_examples = False
            should_run_cli_contracts = False
            should_run_format_check = False
            should_run_benchmark_check = False
            should_run_tool_tests = False

        if args.category == "doc_examples":
            doc_examples_command = [sys.executable, str(root / "scripts" / "check_doc_examples.py"), "--build", regression_build]
            if args.verbose:
                doc_examples_command.append("--verbose")
            return run_command(doc_examples_command, root)

        if args.category == "cli_contracts":
            cli_contracts_command = [sys.executable, str(root / "scripts" / "check_cli_contracts.py"), "--build", regression_build]
            if args.verbose:
                cli_contracts_command.append("--verbose")
            return run_command(cli_contracts_command, root)

        if args.category == "format_check":
            format_check_command = [sys.executable, str(root / "scripts" / "check_format.py"), "--build", regression_build]
            if args.verbose:
                format_check_command.append("--verbose")
            return run_command(format_check_command, root)

        if args.category == "benchmarks":
            benchmark_check_command = [sys.executable, str(root / "scripts" / "check_benchmarks.py"), "--build", regression_build]
            if args.verbose:
                benchmark_check_command.append("--verbose")
            return run_command(benchmark_check_command, root)

        if should_run_doc_examples:
            doc_examples_command = [sys.executable, str(root / "scripts" / "check_doc_examples.py"), "--build", regression_build]
            if args.verbose:
                doc_examples_command.append("--verbose")

            doc_examples_exit_code = run_command(doc_examples_command, root)
            if doc_examples_exit_code != 0:
                return doc_examples_exit_code

        if should_run_cli_contracts:
            cli_contracts_command = [sys.executable, str(root / "scripts" / "check_cli_contracts.py"), "--build", regression_build]
            if args.verbose:
                cli_contracts_command.append("--verbose")

            cli_contracts_exit_code = run_command(cli_contracts_command, root)
            if cli_contracts_exit_code != 0:
                return cli_contracts_exit_code

        if should_run_format_check:
            format_check_command = [sys.executable, str(root / "scripts" / "check_format.py"), "--build", regression_build]
            if args.verbose:
                format_check_command.append("--verbose")

            format_check_exit_code = run_command(format_check_command, root)
            if format_check_exit_code != 0:
                return format_check_exit_code

        if should_run_benchmark_check:
            benchmark_check_command = [sys.executable, str(root / "scripts" / "check_benchmarks.py"), "--build", regression_build]
            if args.verbose:
                benchmark_check_command.append("--verbose")

            benchmark_check_exit_code = run_command(benchmark_check_command, root)
            if benchmark_check_exit_code != 0:
                return benchmark_check_exit_code

        if should_run_tool_tests:
            tool_tests_exit_code = run_tool_tests(root, binary_dir)
            if tool_tests_exit_code != 0:
                return tool_tests_exit_code

        regression_command = [sys.executable, str(root / "scripts" / "run_tests.py"), "--build", regression_build]
        if args.category:
            regression_command.extend(["--category", args.category])
        if args.pattern:
            regression_command.extend(["--pattern", args.pattern])
        if args.test:
            regression_command.extend(["--test", args.test])
        if args.verbose:
            regression_command.append("--verbose")

        regression_exit_code = run_command(regression_command, root)
        if regression_exit_code != 0:
            return regression_exit_code

    print("\nAll requested test steps completed successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
