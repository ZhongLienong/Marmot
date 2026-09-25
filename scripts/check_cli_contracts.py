#!/usr/bin/env python3
"""
Run CLI-facing contract checks that are not represented as plain .mmt fixtures.

Covers:
- `marmotc.exe check <file> --format json`, with MARMOT_PATH
- `marmotc.exe build <file>`, and `marmotvm.exe` running what it built
- `marmotc.exe check|build --plan`, and native libraries loaded by marmotvm
- `marmotc.exe fmt --check`
- `marmotc.exe --version` and `help <command>`, and marmotvm's command line

Projects (manifests, lockfiles, packages, init) and `marmot run`/`marmot test`
belong to the marmot tool and are covered by its tests (tool/tests).
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

from run_tests import TestRunner, vm_beside


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


def run_vm(
    runner: TestRunner,
    args: list[str],
    env_overrides: dict[str, str | None] | None = None,
) -> subprocess.CompletedProcess[str]:
    """marmotvm, beside the marmotc under test."""
    env = os.environ.copy()
    for key, value in (env_overrides or {}).items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    return subprocess.run(
        [str(vm_beside(runner.midori_exe)), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        cwd=repo_root(),
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


def scenario_check_reads_only_regular_files(runner: TestRunner) -> None:
    # Linux opens a directory as an empty stream, and inserting an empty file's
    # buffer set failbit: an empty file was "Could not read file to buffer".
    with tempfile.TemporaryDirectory(prefix="marmot-cli-read-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        empty_path = temp_dir / "Empty.mmt"
        write_text(empty_path, "")
        empty = run_midori(runner, ["check", str(empty_path)], env_overrides={"MARMOT_PATH": None})
        empty_output = empty.stdout + empty.stderr
        assert_condition(empty.returncode != 0 and "Module declaration required" in empty_output, f"check_reads_only_regular_files: an empty file should reach the module check:\n{empty_output}")

        directory_path = temp_dir / "Folder.mmt"
        directory_path.mkdir()
        directory = run_midori(runner, ["check", str(directory_path)], env_overrides={"MARMOT_PATH": None})
        directory_output = directory.stdout + directory.stderr
        assert_condition(directory.returncode != 0 and "Could not open file" in directory_output, f"check_reads_only_regular_files: a directory should not open:\n{directory_output}")


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
    assert_condition("fmt" in completed.stdout and "check" in completed.stdout and "build" in completed.stdout, f"Unexpected help output:\n{completed.stdout}")

    # Projects moved to the marmot tool; running and testing to marmotvm and the tool.
    for removed in ("init", "install", "update", "remove", "list", "run", "test"):
        assert_condition(f"  {removed} " not in completed.stdout, f"help should not list {removed}:\n{completed.stdout}")
        moved = run_midori(runner, [removed], env_overrides={"MARMOT_PATH": None})
        assert_condition(moved.returncode != 0, f"marmotc {removed} should be an unknown command.")

    per_command = run_midori(runner, ["help", "build"], env_overrides={"MARMOT_PATH": None})
    assert_condition(per_command.returncode == 0, f"help_build: expected exit code 0, got {per_command.returncode}.")
    for flag in ("--plan", "-o", "--embed-sources", "--quiet"):
        assert_condition(flag in per_command.stdout, f"Expected {flag} in the build help:\n{per_command.stdout}")


def scenario_marmotvm_runs_what_marmotc_built(runner: TestRunner) -> None:
    with tempfile.TemporaryDirectory(prefix="marmot-cli-run-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)
        source_path = temp_dir / "Main.mmt"
        write_text(
            source_path,
            "module Main\n"
            "def main = fn() -> Int => 0;\n",
        )
        program = temp_dir / "Main.mmc"
        built = run_midori(runner, ["build", str(source_path), "-o", str(program), "--quiet"], env_overrides={"MARMOT_PATH": None})
        assert_condition(built.returncode == 0 and program.exists(), f"build: expected {program}: {built.stdout}{built.stderr}")

        completed = run_vm(runner, [str(program), "--format", "json"])
        payload = parse_command_json("marmotvm_runs_what_marmotc_built", completed)
        report = require_report(payload, "marmotvm_runs_what_marmotc_built")
        assert_condition(completed.returncode == 0, f"marmotvm: expected exit code 0, got {completed.returncode}. Errors: {report['errors']}")
        assert_condition(payload.get("source") == "marmotvm" and payload.get("command") == "run", f"Unexpected payload: {payload}")
        assert_condition(payload.get("success") is True, f"Expected run success payload, got {payload}")
        assert_condition(report["errors"] == [], f"Expected no run errors, got: {report['errors']}")

        # A runtime error is the run's report and its exit status.
        write_text(source_path, "module Main\ndef xs = [1];\ndef x = xs[5];\n")
        run_midori(runner, ["build", str(source_path), "-o", str(program), "--quiet"], env_overrides={"MARMOT_PATH": None})
        failed = run_vm(runner, [str(program), "--format", "json"])
        payload = parse_command_json("marmotvm_runs_what_marmotc_built", failed)
        assert_condition(failed.returncode != 0 and payload.get("success") is False, f"Expected a failed run: {payload}")
        assert_condition(payload["report"]["errors"][0]["code"] == "IndexOutOfBounds", f"Unexpected errors: {payload['report']}")


def scenario_marmotvm_command_line(runner: TestRunner) -> None:
    version = run_vm(runner, ["--version"])
    assert_condition(version.returncode == 0 and re.fullmatch(r"marmotvm \d+\.\d+\.\d+\s*", version.stdout) is not None, f"Unexpected version output:\n{version.stdout}{version.stderr}")

    missing = run_vm(runner, [])
    assert_condition(missing.returncode != 0 and "Missing the program to run" in missing.stderr, f"marmotvm without a program: {missing.stdout}{missing.stderr}")

    unreadable = run_vm(runner, [str(repo_root() / "no-such-program.mmc")])
    assert_condition(unreadable.returncode != 0 and unreadable.stderr.strip() != "", f"marmotvm with no such file: {unreadable.stdout}{unreadable.stderr}")

    malformed = run_vm(runner, ["x.mmc", "--library", "no-equals-sign"])
    assert_condition(malformed.returncode != 0 and "--library takes <name>=<file>" in malformed.stderr, f"marmotvm --library: {malformed.stdout}{malformed.stderr}")


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
        assert_condition(artifact_file.suffix == ".mmc" and artifact_file.exists(), f"Expected the .mmc to exist: {artifact_file}")
        assert_condition(artifact.get("entryFile", "").endswith("Main.mmt"), f"Unexpected artifact entry file: {artifact}")

        # -o puts the program anywhere, making the directories it needs.
        elsewhere = temp_dir / "target" / "deep" / "Main.mmc"
        built_elsewhere = run_midori(runner, ["build", str(source_path), "-o", str(elsewhere), "--quiet"], env_overrides={"MARMOT_PATH": None})
        assert_condition(built_elsewhere.returncode == 0 and elsewhere.exists(), f"build -o: expected {elsewhere}: {built_elsewhere.stdout}{built_elsewhere.stderr}")
        assert_condition(built_elsewhere.stdout.strip() == "", f"build --quiet: expected no summary, got: {built_elsewhere.stdout}")

        # --deps lists what the program was built from: the entry and its imports.
        deps = temp_dir / "Main.deps"
        write_text(
            temp_dir / "lib" / "Support.mmt",
            "module Support\n"
            "public export { Value }\n"
            "def Value = fn() -> Int => 1;\n",
        )
        write_text(
            source_path,
            "module Main\n"
            "import { <Support> }\n"
            "def main = fn() -> Int => Support::Value();\n",
        )
        listed = run_midori(
            runner,
            ["build", str(source_path), "--deps", str(deps), "--quiet"],
            env_overrides={"MARMOT_PATH": str(temp_dir / "lib")},
        )
        assert_condition(listed.returncode == 0, f"build --deps failed: {listed.stdout}{listed.stderr}")
        built_from = sorted(Path(line).name for line in read_text(deps).splitlines() if line.strip() != "")
        assert_condition(built_from == ["Main.mmt", "Support.mmt"], f"Expected the entry and its import, got: {built_from}")

def scenario_native_library_loads_only_to_run(runner: TestRunner) -> None:
    # A module names a native library in source; the run is told where its
    # file is, and that file is not a loadable library. Checking a program that
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
                "native_libraries": [{"name": "native_stub", "thread_safe": True}],
            }),
        )

        checked = run_midori(runner, ["check", "--plan", str(plan_path), "--format", "json"], env_overrides={"MARMOT_PATH": None})
        check_report = require_report(parse_command_json("native_library_loads_only_to_run", checked), "native_library_loads_only_to_run")
        assert_condition(checked.returncode == 0, f"native_library_loads_only_to_run: check should not load the library, got exit {checked.returncode}: {check_report['errors']}")

        program = temp_dir / "Main.mmc"
        built = run_midori(runner, ["build", "--plan", str(plan_path), "-o", str(program), "--quiet"], env_overrides={"MARMOT_PATH": None})
        assert_condition(built.returncode == 0, f"native_library_loads_only_to_run: build should not load the library: {built.stdout}{built.stderr}")

        ran = run_vm(runner, [str(program), "--library-path", str(temp_dir / "stub"), "--format", "json"])
        run_report = require_report(parse_command_json("native_library_loads_only_to_run", ran), "native_library_loads_only_to_run")
        assert_condition(ran.returncode != 0, "native_library_loads_only_to_run: run should fail to load the library.")
        errors = run_report["errors"]
        assert_condition(len(errors) == 1, f"Expected one load error, got: {errors}")
        assert_condition("FFI error" in str(errors[0]["message"]), f"Expected an FFI load error, got: {errors[0]}")

        # Without --library-path nothing says where the library is: the program still
        # compiles, and the run stops because the library is not found.
        unplanned_env = {"MARMOT_PATH": str(package_dir), "MARMOT_LIBRARY_PATH": None}
        unplanned_check = run_midori(runner, ["check", str(temp_dir / "app" / "Main.mmt"), "--format", "json"], env_overrides=unplanned_env)
        assert_condition(unplanned_check.returncode == 0, f"Without a plan, the program should still compile: {unplanned_check.stdout}{unplanned_check.stderr}")
        unplanned_program = temp_dir / "Unplanned.mmc"
        run_midori(runner, ["build", str(temp_dir / "app" / "Main.mmt"), "-o", str(unplanned_program), "--quiet"], env_overrides=unplanned_env)
        unplanned = run_vm(runner, [str(unplanned_program), "--format", "json"], env_overrides=unplanned_env)
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

        for command in ("check", "build"):
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

        # Several paths in one call: a file and a directory, checked together.
        write_text(temp_dir / "lib" / "Lib.mmt", "module Lib\ndef x = fn()->Int=>1;\n")
        several = run_midori(runner, ["fmt", str(source_path), str(temp_dir / "lib"), "--check", "--format", "json"], env_overrides={"MARMOT_PATH": None})
        several_payload = parse_command_json("fmt_several_paths", several)
        assert_condition(several.returncode != 0, "fmt_several_paths: the unformatted file in lib should fail the check.")
        assert_condition(len(several_payload.get("files", [])) == 2 and several_payload.get("changedCount") == 1, f"Expected two files, one changed, got: {several_payload}")

        printed = run_midori(runner, ["fmt", str(source_path), str(temp_dir / "lib")], env_overrides={"MARMOT_PATH": None})
        assert_condition(printed.returncode != 0 and "requires --write or --check" in printed.stdout + printed.stderr, f"fmt_several_paths: printing several paths should be refused: {printed.stdout}{printed.stderr}")


SCENARIOS: list[tuple[str, Any]] = [
    ("check_json_success_finds_imports_through_marmot_path", scenario_check_json_success_finds_imports_through_marmot_path),
    ("check_json_failure_reports_parser_errors", scenario_check_json_failure_reports_parser_errors),
    ("check_reads_only_regular_files", scenario_check_reads_only_regular_files),
    ("version_output_format", scenario_version_output_format),
    ("help_lists_new_commands", scenario_help_lists_new_commands),
    ("marmotvm_runs_what_marmotc_built", scenario_marmotvm_runs_what_marmotc_built),
    ("marmotvm_command_line", scenario_marmotvm_command_line),
    ("build_command_compiles_without_running", scenario_build_command_compiles_without_running),
    ("native_library_loads_only_to_run", scenario_native_library_loads_only_to_run),
    ("plan_replaces_discovery", scenario_plan_replaces_discovery),
    ("fmt_check_and_write", scenario_fmt_check_and_write),
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
