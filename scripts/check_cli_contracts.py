#!/usr/bin/env python3
"""
Run CLI-facing contract checks that are not represented as plain .mmt fixtures.

Covers:
- `marmotc.exe check <file> --format json`, with MARMOT_PATH
- `marmotc.exe run <file>` and `build <file>`
- `marmotc.exe run|check|build --plan`, and native packages from a plan
- `marmotc.exe fmt --check`
- `marmotc.exe test` with --dir, --timeout-ms and --plan
- `marmotc.exe --version` and `help <command>`

Projects (manifests, lockfiles, packages, init) belong to the marmot tool and
are covered by its tests (tool/tests).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

from run_tests import TestRunner


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def run_midori(
    runner: TestRunner,
    args: list[str],
    env_overrides: dict[str, str | None] | None = None,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    if env_overrides is not None:
        for key, value in env_overrides.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value

    return subprocess.run(
        [str(runner.midori_exe), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        cwd=cwd or repo_root(),
        env=env,
        check=False,
    )


def parse_command_json(name: str, completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    if completed.stderr.strip() != "":
        raise AssertionError(f"{name}: expected empty stderr, got:\n{completed.stderr}")

    output = completed.stdout.strip()
    if output == "":
        raise AssertionError(f"{name}: expected JSON on stdout, got empty output.")

    try:
        payload = json.loads(output)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"{name}: stdout was not valid JSON.\n{output}") from exc

    if not isinstance(payload, dict):
        raise AssertionError(f"{name}: expected top-level JSON object, got {type(payload).__name__}.")
    return payload


def require_report(payload: dict[str, Any], name: str) -> dict[str, Any]:
    report = payload.get("report")
    if not isinstance(report, dict):
        raise AssertionError(f"{name}: expected nested report object, got {type(report).__name__}.")
    if "warnings" not in report or "errors" not in report:
        raise AssertionError(f"{name}: report payload is missing warnings/errors keys.")
    return report


def assert_condition(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def scenario_check_json_success_finds_imports_through_marmot_path(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-success-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        library_dir = temp_dir / "lib"
        write_text(
            library_dir / "Support.mmt",
            "module Support\n"
            "public export { Value }\n"
            "def Value = fn() -> Int => 41;\n",
        )
        main_path = temp_dir / "src" / "Main.mmt"
        write_text(
            main_path,
            "module Main\n"
            "import { <Support> }\n"
            "def main = fn() -> Int => {\n"
            "    def unused = 1;\n"
            "    Support::Value()\n"
            "};\n",
        )
        # A project manifest is not marmotc's business: it must not change what
        # the import finds.
        write_text(temp_dir / "project.marmot", "[project]\nsource_dir = \"nowhere\"\n")

        completed = run_midori(
            runner,
            ["check", str(main_path), "--format", "json"],
            env_overrides={"MARMOT_PATH": str(library_dir)},
        )
        payload = parse_command_json("check_json_success_finds_imports_through_marmot_path", completed)
        report = require_report(payload, "check_json_success_finds_imports_through_marmot_path")

        assert_condition(
            completed.returncode == 0,
            f"check_json_success_finds_imports_through_marmot_path: expected exit code 0, got {completed.returncode}. Errors: {report['errors']}",
        )
        assert_condition(payload.get("command") == "check", f"Unexpected command payload: {payload}")
        warnings = report["warnings"]
        errors = report["errors"]
        assert_condition(len(errors) == 0, f"Expected no errors, got: {errors}")
        assert_condition(len(warnings) == 1, f"Expected exactly one warning, got: {warnings}")
        warning = warnings[0]
        assert_condition(warning["stage"] == "StaticAnalyzer", f"Unexpected warning stage: {warning}")
        assert_condition(warning["code"] == "UnusedLocal", f"Unexpected warning code: {warning}")
        assert_condition(str(warning["file_path"]).endswith("Main.mmt"), f"Unexpected warning file_path: {warning}")


def scenario_check_json_failure_reports_parser_errors(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-failure-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        source_path = temp_dir / "Broken.mmt"
        write_text(
            source_path,
            "module Broken\n"
            "def value = ;\n",
        )

        completed = run_midori(
            runner,
            ["check", str(source_path), "--format", "json"],
            env_overrides={"MARMOT_PATH": None},
        )
        payload = parse_command_json("check_json_failure_reports_parser_errors", completed)
        report = require_report(payload, "check_json_failure_reports_parser_errors")

        assert_condition(
            completed.returncode != 0,
            "check_json_failure_reports_parser_errors: expected non-zero exit code.",
        )
        warnings = report["warnings"]
        errors = report["errors"]
        assert_condition(warnings == [], f"Expected no warnings, got: {warnings}")
        assert_condition(isinstance(errors, list) and len(errors) == 1, f"Expected one error, got: {errors}")
        error = errors[0]
        assert_condition(error["stage"] == "Parser", f"Unexpected parser error payload: {error}")
        assert_condition(
            "Expected expression" in str(error["message"]),
            f"Expected parser message to mention 'Expected expression', got: {error}",
        )
        assert_condition(str(error["file_path"]).endswith("Broken.mmt"), f"Unexpected file_path: {error}")


def scenario_version_output_format(runner: TestRunner) -> None:
    completed = run_midori(runner, ["--version"], env_overrides={"MARMOT_PATH": None})
    assert_condition(completed.returncode == 0, f"version_output_format: expected exit code 0, got {completed.returncode}.")
    assert_condition(completed.stderr.strip() == "", f"Expected empty stderr, got:\n{completed.stderr}")
    assert_condition(
        re.fullmatch(r"marmotc \d+\.\d+\.\d+\s*", completed.stdout) is not None,
        f"Unexpected version output:\n{completed.stdout}",
    )


def scenario_help_lists_new_commands(runner: TestRunner) -> None:
    completed = run_midori(runner, [], env_overrides={"MARMOT_PATH": None})
    assert_condition(completed.returncode == 0, f"help_lists_new_commands: expected exit code 0, got {completed.returncode}.")
    assert_condition("fmt" in completed.stdout and "test" in completed.stdout and "build" in completed.stdout, f"Unexpected help output:\n{completed.stdout}")

    for removed in ("init", "install", "update", "remove", "list"):
        assert_condition(f"  {removed} " not in completed.stdout, f"help should not list {removed}, which moved to the marmot tool:\n{completed.stdout}")
    moved = run_midori(runner, ["install"], env_overrides={"MARMOT_PATH": None})
    assert_condition(moved.returncode != 0, "marmotc install should be an unknown command.")

    per_command = run_midori(runner, ["help", "test"], env_overrides={"MARMOT_PATH": None})
    assert_condition(per_command.returncode == 0, f"help_test: expected exit code 0, got {per_command.returncode}.")
    for flag in ("--pattern", "--test", "--dir", "--timeout-ms", "--plan"):
        assert_condition(flag in per_command.stdout, f"Expected {flag} in the test help:\n{per_command.stdout}")


def scenario_run_command_executes_program(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-run-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        source_path = temp_dir / "Main.mmt"
        write_text(
            source_path,
            "module Main\n"
            "def main = fn() -> Int => 0;\n",
        )

        completed = run_midori(
            runner,
            ["run", str(source_path), "--format", "json"],
            env_overrides={"MARMOT_PATH": None},
        )
        payload = parse_command_json("run_command_executes_program", completed)
        report = require_report(payload, "run_command_executes_program")

        assert_condition(completed.returncode == 0, f"run_command_executes_program: expected exit code 0, got {completed.returncode}. Errors: {report['errors']}")
        assert_condition(payload.get("command") == "run", f"Unexpected command payload: {payload}")
        assert_condition(payload.get("success") is True, f"Expected run success payload, got {payload}")
        assert_condition(report["errors"] == [], f"Expected no run errors, got: {report['errors']}")


def scenario_build_command_compiles_without_running(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-build-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        source_path = temp_dir / "Main.mmt"
        write_text(
            source_path,
            "module Main\n"
            "def main = fn() -> Int => 13;\n",
        )

        completed = run_midori(
            runner,
            ["build", str(source_path), "--format", "json"],
            env_overrides={"MARMOT_PATH": None},
        )
        payload = parse_command_json("build_command_compiles_without_running", completed)
        report = require_report(payload, "build_command_compiles_without_running")

        assert_condition(completed.returncode == 0, f"build_command_compiles_without_running: expected exit code 0, got {completed.returncode}. Errors: {report['errors']}")
        assert_condition(payload.get("command") == "build", f"Unexpected command payload: {payload}")
        assert_condition(payload.get("success") is True, f"Expected build success payload, got {payload}")
        assert_condition(report["errors"] == [], f"Expected no build errors, got: {report['errors']}")
        artifact = payload.get("artifact")
        assert_condition(isinstance(artifact, dict), f"Expected artifact object, got: {artifact}")
        assert_condition(artifact.get("procedureCount", 0) >= 1, f"Expected procedure count in artifact, got: {artifact}")
        artifact_path = artifact.get("path")
        assert_condition(isinstance(artifact_path, str) and artifact_path != "", f"Expected artifact path, got: {artifact}")
        artifact_file = Path(artifact_path)
        assert_condition(artifact_file.exists(), f"Expected artifact file to exist: {artifact_file}")
        artifact_payload = json.loads(read_text(artifact_file))
        assert_condition(isinstance(artifact_payload.get("procedures"), list), f"Expected serialized procedures in artifact: {artifact_payload}")
        assert_condition(artifact_payload.get("entryFile", "").endswith("Main.mmt"), f"Unexpected artifact entry file: {artifact_payload}")


def scenario_native_library_loads_only_to_run(runner: TestRunner) -> None:
    # A module names a native library in source; the plan says where its file
    # is, and that file is not a loadable library. Checking a program that
    # imports the module must not touch the library (loading one runs its
    # code); running it must stop before it starts, with the load failure
    # reported like a compile error.
    with tempfile.TemporaryDirectory(prefix="marmot-cli-native-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        package_dir = temp_dir / "Native"
        write_text(
            package_dir / "Native.mmt",
            "module Native\n"
            "public export { Answer }\n"
            "foreign \"native_answer\" Answer : fn() -> Int from \"native_stub\";\n",
        )
        write_text(temp_dir / "stub" / "native_stub.dll", "not a library\n")
        write_text(
            temp_dir / "app" / "Main.mmt",
            "module Main\n"
            "import { \"<Native>\" }\n"
            "def main = fn() -> Int => 0;\n",
        )
        plan_path = temp_dir / "plan.json"
        write_text(
            plan_path,
            json.dumps({
                "version": 1,
                "entry": "app/Main.mmt",
                "search_paths": ["Native"],
                "native_libraries": [{"name": "native_stub", "path": "stub/native_stub.dll"}],
            }),
        )

        checked = run_midori(runner, ["check", "--plan", str(plan_path), "--format", "json"], env_overrides={"MARMOT_PATH": None})
        check_report = require_report(parse_command_json("native_library_loads_only_to_run", checked), "native_library_loads_only_to_run")
        assert_condition(checked.returncode == 0, f"native_library_loads_only_to_run: check should not load the library, got exit {checked.returncode}: {check_report['errors']}")

        ran = run_midori(runner, ["run", "--plan", str(plan_path), "--format", "json"], env_overrides={"MARMOT_PATH": None})
        run_report = require_report(parse_command_json("native_library_loads_only_to_run", ran), "native_library_loads_only_to_run")
        assert_condition(ran.returncode != 0, "native_library_loads_only_to_run: run should fail to load the library.")
        errors = run_report["errors"]
        assert_condition(len(errors) == 1, f"Expected one load error, got: {errors}")
        assert_condition("FFI error" in str(errors[0]["message"]), f"Expected an FFI load error, got: {errors[0]}")

        # Without the plan nothing says where the library is: the program still
        # compiles, and the run stops because the library is not found.
        unplanned_env = {"MARMOT_PATH": str(package_dir), "MARMOT_LIBRARY_PATH": None}
        unplanned_check = run_midori(runner, ["check", str(temp_dir / "app" / "Main.mmt"), "--format", "json"], env_overrides=unplanned_env)
        assert_condition(unplanned_check.returncode == 0, f"Without a plan, the program should still compile: {unplanned_check.stdout}{unplanned_check.stderr}")
        unplanned = run_midori(runner, ["run", str(temp_dir / "app" / "Main.mmt"), "--format", "json"], env_overrides=unplanned_env)
        unplanned_report = require_report(parse_command_json("native_library_loads_only_to_run", unplanned), "native_library_loads_only_to_run")
        assert_condition(unplanned.returncode != 0, "Without a plan, the run should not find the library.")
        assert_condition(any("native library 'native_stub' not found" in str(error["message"]) for error in unplanned_report["errors"]), f"Unexpected errors: {unplanned_report['errors']}")


def scenario_plan_replaces_discovery(runner: TestRunner) -> None:
    # A build plan is the whole input: its search paths are used, MARMOT_PATH is
    # not, and a misspelt member is an error rather than a silently lost input.
    with tempfile.TemporaryDirectory(prefix="marmot-cli-plan-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        write_text(
            temp_dir / "src" / "Main.mmt",
            "module Main\n"
            "import { \"<Greeting>\" }\n"
            "def main = fn() -> Int => Greeting::Answer();\n",
        )
        write_text(
            temp_dir / "lib" / "Greeting.mmt",
            "module Greeting\n"
            "public export { Answer }\n"
            "def Answer = fn() -> Int => 0;\n",
        )
        # A decoy that would satisfy the import if MARMOT_PATH were read.
        write_text(
            temp_dir / "decoy" / "Greeting.mmt",
            "module Greeting\n"
            "def broken = ;\n",
        )
        plan_path = temp_dir / "plan.json"
        write_text(
            plan_path,
            json.dumps({"version": 1, "entry": "src/Main.mmt", "search_paths": ["lib"]}),
        )
        env = {"MARMOT_PATH": str(temp_dir / "decoy")}

        for command in ("check", "run"):
            completed = run_midori(runner, [command, "--plan", str(plan_path), "--format", "json"], env_overrides=env)
            report = require_report(parse_command_json("plan_replaces_discovery", completed), "plan_replaces_discovery")
            assert_condition(completed.returncode == 0, f"plan_replaces_discovery: {command} --plan failed with exit {completed.returncode}: {report['errors']}")

        built = run_midori(runner, ["build", "--plan", str(plan_path)], env_overrides=env)
        assert_condition(built.returncode == 0, f"plan_replaces_discovery: build --plan failed: {built.stdout}{built.stderr}")
        assert_condition((temp_dir / "src" / "Main.mmc").exists(), "plan_replaces_discovery: build --plan should write src/Main.mmc beside the entry.")

        both = run_midori(runner, ["check", str(temp_dir / "src" / "Main.mmt"), "--plan", str(plan_path)], env_overrides=env)
        assert_condition(both.returncode != 0 and "not both" in both.stdout + both.stderr, f"plan_replaces_discovery: a file and --plan together should be refused: {both.stdout}{both.stderr}")

        write_text(plan_path, json.dumps({"version": 1, "entry": "src/Main.mmt", "serach_paths": ["lib"]}))
        misspelt = run_midori(runner, ["check", "--plan", str(plan_path)], env_overrides=env)
        output = misspelt.stdout + misspelt.stderr
        assert_condition(misspelt.returncode != 0, "plan_replaces_discovery: a misspelt plan member should fail.")
        assert_condition("unknown member \"serach_paths\"" in output, f"plan_replaces_discovery: expected the misspelt member to be named, got: {output}")


def scenario_fmt_check_and_write(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-fmt-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        source_path = temp_dir / "Main.mmt"
        write_text(
            source_path,
            "module Main\n"
            "// comment\n"
            "def main = fn()->Int=>0; // trailing\n",
        )

        check_before = run_midori(runner, ["fmt", str(source_path), "--check", "--format", "json"], env_overrides={"MARMOT_PATH": None})
        check_before_payload = parse_command_json("fmt_check_before_write", check_before)
        assert_condition(check_before.returncode != 0, "fmt_check_before_write: expected non-zero exit code.")
        assert_condition(check_before_payload.get("changedCount") == 1, f"Expected one changed file before write, got: {check_before_payload}")

        write_completed = run_midori(runner, ["fmt", str(source_path), "--write", "--format", "json"], env_overrides={"MARMOT_PATH": None})
        write_payload = parse_command_json("fmt_write", write_completed)
        assert_condition(write_completed.returncode == 0, f"fmt_write: expected exit code 0, got {write_completed.returncode}.")
        assert_condition(write_payload.get("changedCount") == 1, f"Expected one changed file during write, got: {write_payload}")
        formatted_text = read_text(source_path)
        assert_condition("// comment" in formatted_text, f"Expected leading comment to be preserved, got:\n{formatted_text}")
        assert_condition("// trailing" in formatted_text, f"Expected trailing comment to be preserved, got:\n{formatted_text}")

        check_after = run_midori(runner, ["fmt", str(source_path), "--check", "--format", "json"], env_overrides={"MARMOT_PATH": None})
        check_after_payload = parse_command_json("fmt_check_after_write", check_after)
        assert_condition(check_after.returncode == 0, f"fmt_check_after_write: expected exit code 0, got {check_after.returncode}.")
        assert_condition(check_after_payload.get("changedCount") == 0, f"Expected no changed files after write, got: {check_after_payload}")


def scenario_test_command_discovers_tests(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-test-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        write_text(
            temp_dir / "test" / "smoke.mmt",
            "module Smoke\n"
            "def main = fn() -> Int => 0;\n",
        )
        write_text(
            temp_dir / "checks" / "uses_lib.mmt",
            "module UsesLib\n"
            "import { <Support> }\n"
            "def main = fn() -> Int => Support::Value();\n",
        )
        write_text(
            temp_dir / "lib" / "Support.mmt",
            "module Support\n"
            "public export { Value }\n"
            "def Value = fn() -> Int => 0;\n",
        )
        write_text(temp_dir / "plan.json", json.dumps({"version": 1, "search_paths": ["lib"]}))

        # Default: ./test under the current directory.
        completed = run_midori(runner, ["test", "--format", "json"], env_overrides={"MARMOT_PATH": None}, cwd=temp_dir)
        payload = parse_command_json("test_command_discovers_tests", completed)
        summary = payload.get("summary")
        assert_condition(completed.returncode == 0, f"test_command_discovers_tests: expected exit code 0, got {completed.returncode}. Payload: {payload}")
        assert_condition(payload.get("command") == "test", f"Unexpected command payload: {payload}")
        assert_condition(isinstance(summary, dict) and summary.get("total") == 1 and summary.get("passed") == 1, f"Expected one passing test, got: {payload}")

        # --dir and --plan: every test compiles with the plan's search paths,
        # in the worker processes too.
        completed = run_midori(runner, ["test", "--dir", "checks", "--plan", str(temp_dir / "plan.json"), "--format", "json"], env_overrides={"MARMOT_PATH": None}, cwd=temp_dir)
        payload = parse_command_json("test_command_discovers_tests", completed)
        summary = payload.get("summary")
        assert_condition(completed.returncode == 0, f"test_command_discovers_tests: --plan run failed. Payload: {payload}")
        assert_condition(isinstance(summary, dict) and summary.get("total") == 1 and summary.get("passed") == 1, f"Expected one passing test, got: {payload}")


def scenario_test_command_enforces_timeout(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-test-timeout-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        project_dir = temp_dir / "Project"
        test_dir = project_dir / "test"
        test_dir.mkdir(parents=True)

        write_text(
            test_dir / "hang.mmt",
            "module Hang\n"
            "// `loop` was removed in v2; a tail call recurses forever without growing the stack.\n"
            "def Spin = fn(n: Int) -> Int => Spin(n + 1);\n"
            "Spin(0);\n",
        )

        completed = run_midori(
            runner,
            ["test", "--timeout-ms", "50", "--format", "json"],
            env_overrides={"MARMOT_PATH": None},
            cwd=project_dir,
        )
        payload = parse_command_json("test_command_enforces_timeout", completed)
        summary = payload.get("summary")
        results = payload.get("results")

        assert_condition(completed.returncode != 0, "test_command_enforces_timeout: expected non-zero exit code.")
        assert_condition(isinstance(summary, dict), f"Expected summary object, got: {payload}")
        assert_condition(summary.get("total") == 1, f"Expected one discovered test, got: {payload}")
        assert_condition(summary.get("timedOut") == 1, f"Expected one timed out test, got: {payload}")
        assert_condition(isinstance(results, list) and len(results) == 1, f"Expected one result entry, got: {payload}")
        assert_condition(results[0].get("timedOut") is True, f"Expected timedOut=true on the test result, got: {results[0]}")

SCENARIOS: list[tuple[str, Any]] = [
    ("check_json_success_finds_imports_through_marmot_path", scenario_check_json_success_finds_imports_through_marmot_path),
    ("check_json_failure_reports_parser_errors", scenario_check_json_failure_reports_parser_errors),
    ("version_output_format", scenario_version_output_format),
    ("help_lists_new_commands", scenario_help_lists_new_commands),
    ("run_command_executes_program", scenario_run_command_executes_program),
    ("build_command_compiles_without_running", scenario_build_command_compiles_without_running),
    ("native_library_loads_only_to_run", scenario_native_library_loads_only_to_run),
    ("plan_replaces_discovery", scenario_plan_replaces_discovery),
    ("fmt_check_and_write", scenario_fmt_check_and_write),
    ("test_command_discovers_tests", scenario_test_command_discovers_tests),
    ("test_command_enforces_timeout", scenario_test_command_enforces_timeout),
]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run Marmot CLI contract checks.")
    parser.add_argument(
        "--build",
        default="Development",
        choices=["Debug", "Development", "Release"],
        help="Build configuration used to locate marmotc.exe (default: Development).",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show failure details.",
    )
    args = parser.parse_args(argv)

    runner = TestRunner(build_config=args.build, verbose=args.verbose)
    print(f"Executable: {runner.midori_exe}")
    print(f"Build: {runner.build_config}")
    if runner.executable_notice:
        print(f"Notice: {runner.executable_notice}")

    failures: list[tuple[str, str]] = []
    for name, scenario in SCENARIOS:
        try:
            scenario(runner)
            print(f"[OK] {name}")
        except AssertionError as exc:
            print(f"[FAIL] {name}")
            if args.verbose:
                print(str(exc))
            failures.append((name, str(exc)))

    if failures:
        print(f"\n[FAILED] {len(failures)} CLI contract check(s) failed.")
        if not args.verbose:
            for name, message in failures:
                print(f"- {name}: {message}")
        return 1

    print(f"\n[SUCCESS] {len(SCENARIOS)} CLI contract check(s) passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
