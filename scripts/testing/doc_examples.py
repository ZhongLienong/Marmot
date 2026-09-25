#!/usr/bin/env python3
"""
Extract, sync-check, and compile runnable documentation examples.

Runnable examples are marked with fenced code blocks whose info string starts
with `marmot-test`. Example:

```marmot-test name=readme/hello_world path=.doc_examples/readme/hello_world.mmt
module Main
import { "../MarmotPrelude/IO.mmt" }
IO::PrintLine("Hello, Marmot!");
```

Required metadata:
- `name`: stable mirror path under `test/doc_examples/<kind>/`
- `path`: repo-relative temporary compile path used for the actual extraction run

Optional metadata:
- `kind`: `success` or `failure` (default: `success`)
- `module`: module name to prepend when the snippet intentionally omits it
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT, prelude_search_path
from lib.presets import BuildTree, add_build_arguments
from lib.program import build_and_run
from testing.language import TestRunner


def cleanup_doc_artifacts(root: Path) -> None:
    patterns = (".doc_example_*.mmt", "*.ppm", "*.mmc", "*.mmc.json")
    for pattern in patterns:
        for file in root.glob(pattern):
            if file.is_file():
                try:
                    file.unlink()
                except OSError:
                    pass
    doc_examples_dir = root / ".doc_examples"
    if doc_examples_dir.is_dir():
        shutil.rmtree(doc_examples_dir, ignore_errors=True)


@dataclass(frozen=True)
class DocExample:
    doc_path: Path
    start_line: int
    name: str
    kind: str
    compile_path: Path
    module_name: Optional[str]
    snippet_source: str

    def generated_source(self) -> str:
        if self.module_name is None:
            return self.snippet_source

        if self.snippet_source.lstrip().startswith("module "):
            raise ValueError(
                f"{self.doc_path}:{self.start_line} already declares a module; "
                f"remove module={self.module_name} from the fence metadata."
            )

        return f"module {self.module_name}\n\n{self.snippet_source}"


def repo_root() -> Path:
    return REPO_ROOT


def docs_to_scan(root: Path) -> list[Path]:
    result = [root / "README.md"]
    result.extend(sorted((root / "docs").glob("*.md")))
    return result


def parse_metadata(raw: str, source_path: Path, line_number: int) -> dict[str, str]:
    metadata: dict[str, str] = {}
    if raw.strip() == "":
        raise ValueError(f"{source_path}:{line_number} marmot-test fence requires metadata.")

    for token in shlex.split(raw):
        if "=" not in token:
            raise ValueError(
                f"{source_path}:{line_number} invalid marmot-test token '{token}'. "
                "Use key=value metadata."
            )

        key, value = token.split("=", 1)
        if key == "" or value == "":
            raise ValueError(f"{source_path}:{line_number} invalid marmot-test token '{token}'.")
        metadata[key] = value

    if "name" not in metadata:
        raise ValueError(f"{source_path}:{line_number} marmot-test fence is missing name=...")
    if "path" not in metadata:
        raise ValueError(f"{source_path}:{line_number} marmot-test fence is missing path=...")

    kind = metadata.get("kind", "success")
    if kind not in {"success", "failure"}:
        raise ValueError(
            f"{source_path}:{line_number} invalid kind='{kind}'. "
            "Expected success or failure."
        )

    return metadata


def parse_doc_examples(source_path: Path, root: Path) -> list[DocExample]:
    lines = source_path.read_text(encoding="utf-8").splitlines()
    examples: list[DocExample] = []
    index = 0

    while index < len(lines):
        line = lines[index]
        if not line.startswith("```marmot-test"):
            index += 1
            continue

        metadata = parse_metadata(line[len("```marmot-test"):].strip(), source_path, index + 1)
        block_start_line = index + 2
        index += 1
        block_lines: list[str] = []

        while index < len(lines) and lines[index].strip() != "```":
            block_lines.append(lines[index])
            index += 1

        if index >= len(lines):
            raise ValueError(f"{source_path}:{block_start_line} unterminated marmot-test fence.")

        snippet_source = "\n".join(block_lines).rstrip() + "\n"
        compile_path = (root / metadata["path"]).resolve()
        if root.resolve() not in compile_path.parents and compile_path != root.resolve():
            raise ValueError(
                f"{source_path}:{block_start_line} path={metadata['path']} resolves outside the repository."
            )

        examples.append(
            DocExample(
                doc_path=source_path,
                start_line=block_start_line,
                name=metadata["name"],
                kind=metadata.get("kind", "success"),
                compile_path=compile_path,
                module_name=metadata.get("module"),
                snippet_source=snippet_source,
            )
        )

        index += 1

    return examples


def discover_examples(root: Path) -> list[DocExample]:
    examples: list[DocExample] = []
    seen_names: set[str] = set()

    for doc_path in docs_to_scan(root):
        for example in parse_doc_examples(doc_path, root):
            if example.name in seen_names:
                raise ValueError(f"Duplicate doc example name '{example.name}'.")
            seen_names.add(example.name)
            examples.append(example)

    return examples


def mirror_path(root: Path, example: DocExample) -> Path:
    return root / "test" / "doc_examples" / example.kind / f"{example.name}.mmt"


def expected_snapshot_path(root: Path, example: DocExample) -> Path:
    return mirror_path(root, example).with_suffix(".expected")


def warnings_snapshot_path(root: Path, example: DocExample) -> Path:
    return mirror_path(root, example).with_suffix(".warnings.json")


def normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def allowed_artifact_paths(root: Path, examples: list[DocExample]) -> set[Path]:
    allowed_paths: set[Path] = set()

    for example in examples:
        allowed_paths.add(mirror_path(root, example).resolve())
        allowed_paths.add(expected_snapshot_path(root, example).resolve())
        allowed_paths.add(warnings_snapshot_path(root, example).resolve())

    return allowed_paths


def doc_examples_root(root: Path) -> Path:
    return root / "test" / "doc_examples"


def remove_empty_doc_example_directories(root: Path) -> None:
    artifacts_root = doc_examples_root(root)
    if artifacts_root.exists() is False:
        return

    for directory in sorted(
        (path for path in artifacts_root.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            continue


def find_orphaned_artifacts(root: Path, examples: list[DocExample]) -> list[Path]:
    artifacts_root = doc_examples_root(root)
    if artifacts_root.exists() is False:
        return []

    allowed_paths = allowed_artifact_paths(root, examples)
    orphaned_paths: list[Path] = []

    for path in artifacts_root.rglob("*"):
        if path.is_dir():
            continue
        if path.resolve() in allowed_paths:
            continue
        orphaned_paths.append(path)

    return sorted(orphaned_paths)


def sync_check_or_update(root: Path, examples: list[DocExample], sync: bool) -> list[str]:
    errors: list[str] = []

    for example in examples:
        expected_text = example.generated_source()
        target_path = mirror_path(root, example)

        if sync:
            write_text(target_path, expected_text)
            continue

        if target_path.exists() is False:
            errors.append(
                f"Missing mirrored doc example: {target_path}. "
                "Run python scripts/dev.py check docs --sync after updating docs."
            )
            continue

        actual_text = normalize_newlines(target_path.read_text(encoding="utf-8"))
        if actual_text != expected_text:
            errors.append(
                f"Stale mirrored doc example: {target_path}. "
                "Run python scripts/dev.py check docs --sync after updating docs."
            )

    orphaned_artifacts = find_orphaned_artifacts(root, examples)
    if sync:
        for path in orphaned_artifacts:
            path.unlink()
        remove_empty_doc_example_directories(root)
        return errors

    for path in orphaned_artifacts:
        errors.append(
            f"Orphaned doc example artifact: {path}. "
            "Run python scripts/dev.py check docs --sync after updating docs."
        )

    return errors


def cleanup_temp_file(root: Path, compile_path: Path) -> None:
    try:
        compile_path.unlink(missing_ok=True)
    except OSError:
        return

    root_resolved = root.resolve()
    current = compile_path.parent
    while current != root_resolved:
        try:
            current.rmdir()
        except OSError:
            break
        current = current.parent


def load_expected_warnings(runner: TestRunner, root: Path, example: DocExample) -> Optional[list[dict]]:
    warnings_path = warnings_snapshot_path(root, example)
    if warnings_path.exists() is False:
        return None
    return runner.get_expected_warnings(mirror_path(root, example))


def load_expected_output(root: Path, example: DocExample) -> Optional[str]:
    expected_path = expected_snapshot_path(root, example)
    if expected_path.exists() is False:
        return None
    return expected_path.read_text(encoding="utf-8")


def run_example(root: Path, runner: TestRunner, example: DocExample, verbose: bool) -> Optional[str]:
    compile_source = example.generated_source()
    compile_path = example.compile_path
    expected_output = load_expected_output(root, example)
    expected_warnings = load_expected_warnings(runner, root, example)

    write_text(compile_path, compile_source)
    env = os.environ.copy()
    env["MARMOT_TEST_MODE"] = "1"
    env["MARMOT_PATH"] = prelude_search_path()
    if expected_warnings is not None:
        env["MARMOT_TEST_WARNING_FORMAT"] = "machine"

    command_path = str(compile_path.relative_to(root))
    try:
        completed = build_and_run(runner.midori_exe, command_path, cwd=root, env=env, timeout=30)
    finally:
        cleanup_temp_file(root, compile_path)

    output = completed.stdout + completed.stderr
    actual_warnings, human_output = runner.split_machine_readable_warnings(output)
    normalized_output = runner.normalize_snapshot_text(human_output)

    expected_to_fail = example.kind == "failure"
    passed = completed.returncode != 0 if expected_to_fail else completed.returncode == 0
    failure_reason: Optional[str] = None

    if passed is False:
        expected_status = "non-zero" if expected_to_fail else "zero"
        failure_reason = f"Expected exit code {expected_status}, got {completed.returncode}."

    if passed and expected_output is not None:
        normalized_expected = runner.normalize_snapshot_text(expected_output)
        if normalized_output.strip() != normalized_expected.strip():
            passed = False
            failure_reason = (
                "Output snapshot mismatch.\n"
                f"Expected:\n{normalized_expected}\n\n"
                f"Actual:\n{normalized_output}"
            )

    if passed and expected_warnings is not None:
        if actual_warnings != expected_warnings:
            passed = False
            failure_reason = (
                "Warning snapshot mismatch.\n"
                f"Expected:\n{json.dumps(expected_warnings, indent=2, ensure_ascii=False)}\n\n"
                f"Actual:\n{json.dumps(actual_warnings, indent=2, ensure_ascii=False)}"
            )

    status = "[OK]" if passed else "[FAIL]"
    print(f"{status} {example.kind.upper()} {example.name}")
    if passed is False and (verbose or True):
        print(f"  Source: {example.doc_path}:{example.start_line}")
        print(f"  Mirror: {mirror_path(root, example)}")
        print(f"  Temp:   {compile_path.relative_to(root)}")
        print(f"  Error:  {failure_reason}")

    return None if passed else failure_reason


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Sync-check and compile runnable Markdown Marmot examples.")
    add_build_arguments(parser)
    parser.add_argument(
        "--sync",
        action="store_true",
        help="Rewrite mirrored files under test/doc_examples/ from the current docs before compiling.",
    )
    parser.add_argument(
        "--skip-run",
        action="store_true",
        help="Only perform mirror sync/check; do not compile extracted examples.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show more detail for failures.",
    )
    args = parser.parse_args(argv)

    root = repo_root()
    examples = discover_examples(root)
    if len(examples) == 0:
        print("No marmot-test fences found.")
        return 0

    sync_errors = sync_check_or_update(root, examples, args.sync)
    if sync_errors:
        for error in sync_errors:
            print(f"[FAIL] {error}")
        return 1

    print(f"Discovered {len(examples)} runnable doc example(s).")
    if args.skip_run:
        return 0

    runner = TestRunner(BuildTree.from_args(args), verbose=args.verbose)
    print(f"Executable: {runner.midori_exe}")
    print(f"Build: {runner.build_config}")

    failures = []
    try:
        failures = [error for example in examples if (error := run_example(root, runner, example, args.verbose)) is not None]
    finally:
        cleanup_doc_artifacts(root)

    if failures:
        print(f"\n[FAILED] {len(failures)} doc example(s) failed.")
        return 1

    print(f"\n[SUCCESS] {len(examples)} doc example(s) passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
