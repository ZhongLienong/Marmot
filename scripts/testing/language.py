#!/usr/bin/env python3
"""
The language suite: every .mmt under test/, built with marmotc and run in marmotvm.

- A test under a failure/ folder must fail; any other must succeed.
- A .expected beside a test is its output, compared after stripping colour and
  this checkout's path.
- A .warnings.json beside a test is its warnings, compared as JSON.

Usage:
    python scripts/testing/language.py                          # every test
    python scripts/testing/language.py --category closure       # one folder
    python scripts/testing/language.py --pattern loop           # names containing 'loop'
    python scripts/testing/language.py --test closure/simple    # one test
    python scripts/testing/language.py --build Debug            # another build
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.console import Color
from lib.host import REPO_ROOT, TEST_DIR, checkout_environment
from lib.presets import BuildTree, add_build_arguments
from lib.program import build_and_run

TIMEOUT_SECONDS = 30


def cleanup_language_test_artifacts(root: Path, extra_files: set[Path] | None = None) -> None:
    patterns = (
        "*.ppm",
        "*.mmc",
        "*.mmc.json",
        "midori_phase3_io_*",
        "midori_phase8_doc_*",
    )
    for pattern in patterns:
        for path in root.glob(pattern):
            if path.is_file():
                try:
                    path.unlink()
                except OSError:
                    pass
            elif path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
    for dir_path in root.glob("midori_phase3_missing_dir_*"):
        if dir_path.is_dir():
            shutil.rmtree(dir_path, ignore_errors=True)
    if extra_files:
        for path in extra_files:
            try:
                if path.is_file():
                    path.unlink()
                elif path.is_dir():
                    shutil.rmtree(path, ignore_errors=True)
            except OSError:
                pass


@dataclass
class TestResult:
    """Result of running a single test."""
    name: str
    path: Path
    passed: bool
    expected_to_fail: bool
    output: str
    exit_code: int = 0
    error: Optional[str] = None
    duration_ms: float = 0.0

class TestRunner:
    def __init__(self, tree: BuildTree, verbose: bool = False):
        self.root_dir = REPO_ROOT
        self.test_dir = TEST_DIR
        self.build_config = tree.build_type
        self.verbose = verbose
        self.midori_exe = tree.require_compiler()
        self.results: List[TestResult] = []

    def is_failure_test(self, test_path: Path) -> bool:
        """Check if test is expected to fail based on directory name."""
        return 'failure' in str(test_path.parent).lower()

    def get_expected_output(self, test_path: Path) -> Optional[str]:
        """Get expected output from .expected file if it exists."""
        expected_file = test_path.with_suffix('.expected')
        if expected_file.exists():
            return expected_file.read_text(encoding='utf-8')
        return None

    def get_expected_warnings(self, test_path: Path) -> Optional[List[dict]]:
        """Get expected machine-readable warnings from .warnings.json if it exists."""
        warnings_file = test_path.with_suffix('.warnings.json')
        if not warnings_file.exists():
            return None

        with warnings_file.open(encoding='utf-8') as handle:
            warning_data = json.load(handle)

        if not isinstance(warning_data, list):
            raise ValueError(f"{warnings_file} must contain a JSON array of warning objects")

        normalized_warnings: List[dict] = []
        for warning in warning_data:
            if not isinstance(warning, dict):
                raise ValueError(f"{warnings_file} entries must be JSON objects")
            normalized_warnings.append(self.normalize_warning_record(warning))

        return normalized_warnings

    def normalize_path_text(self, text: str) -> str:
        clean = text.replace('\r\n', '\n').replace('\r', '')

        resolved_root = str(self.root_dir.resolve())
        root_variants = {resolved_root, resolved_root.replace('\\', '/')}
        for root in root_variants:
            clean = clean.replace(root + "\\", "")
            clean = clean.replace(root + "/", "")

        return clean.replace('\\', '/')

    def normalize_snapshot_text(self, text: str) -> str:
        """Normalize diagnostic/output text before comparing it to a snapshot."""
        clean = re.sub(r'\x1b\[[0-9;]*m', '', text)
        clean = self.normalize_path_text(clean)
        clean = re.sub(r'(^\d+ \| .*)\n+(?=\s+\|)', r'\1\n', clean, flags=re.MULTILINE)
        return '\n'.join(line.rstrip() for line in clean.split('\n'))

    def normalize_warning_record(self, warning: dict) -> dict:
        normalized = dict(warning)
        file_name = normalized.get("file")
        if isinstance(file_name, str):
            normalized["file"] = self.normalize_path_text(file_name)
        file_path = normalized.get("file_path")
        if isinstance(file_path, str):
            normalized["file_path"] = self.normalize_path_text(file_path)
        return normalized

    def split_machine_readable_warnings(self, output: str) -> tuple[List[dict], str]:
        warning_records: List[dict] = []
        non_warning_lines: List[str] = []

        for line in output.splitlines():
            if line.startswith("MARMOT_WARNING\t"):
                payload = line.split("\t", 1)[1]
                warning = json.loads(payload)
                if not isinstance(warning, dict):
                    raise ValueError("Machine-readable warning payload must be a JSON object")
                warning_records.append(self.normalize_warning_record(warning))
                continue

            non_warning_lines.append(line)

        return warning_records, '\n'.join(non_warning_lines)

    def execute(self, test_path: Path, *, machine_warnings: bool) -> subprocess.CompletedProcess:
        """Build and run one test as the suite does, from the checkout root."""
        environment = checkout_environment(
            MARMOT_TEST_MODE="1",
            MARMOT_TEST_WARNING_FORMAT="machine" if machine_warnings else None,
        )
        command_path = test_path.resolve().relative_to(self.root_dir).as_posix()
        files_before = {p for p in self.root_dir.iterdir() if p.is_file()}
        try:
            return build_and_run(self.midori_exe, command_path, cwd=self.root_dir, env=environment, timeout=TIMEOUT_SECONDS)
        finally:
            files_after = {p for p in self.root_dir.iterdir() if p.is_file()}
            cleanup_language_test_artifacts(self.root_dir, extra_files=files_after - files_before)

    def run_test(self, test_path: Path) -> TestResult:
        """Run a single test file."""
        relative_path = test_path.relative_to(self.test_dir)
        test_name = str(relative_path)

        expected_to_fail = self.is_failure_test(test_path)
        expected_output = self.get_expected_output(test_path)

        try:
            expected_warnings = self.get_expected_warnings(test_path)
            start = time.time()
            result = self.execute(test_path, machine_warnings=expected_warnings is not None)

            duration_ms = (time.time() - start) * 1000

            output = result.stdout + result.stderr
            actual_warnings, human_output = self.split_machine_readable_warnings(output)
            normalized_output = self.normalize_snapshot_text(human_output)
            failure_reason: Optional[str] = None

            # Determine if test passed
            if expected_to_fail:
                # Failure tests should have non-zero exit code
                passed = result.returncode != 0
            else:
                # Success tests should have zero exit code
                passed = result.returncode == 0

            if not passed:
                expected_status = "non-zero" if expected_to_fail else "zero"
                failure_reason = f"Expected exit code {expected_status}, got {result.returncode}."

            # Compare snapshots for both success and failure tests after normalizing
            if passed and expected_output is not None:
                normalized_expected = self.normalize_snapshot_text(expected_output)
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

            return TestResult(
                name=test_name,
                path=test_path,
                passed=passed,
                expected_to_fail=expected_to_fail,
                output=output,
                exit_code=result.returncode,
                error=failure_reason,
                duration_ms=duration_ms
            )

        except subprocess.TimeoutExpired:
            return TestResult(
                name=test_name,
                path=test_path,
                passed=False,
                expected_to_fail=expected_to_fail,
                output="",
                error=f"Test timed out ({TIMEOUT_SECONDS}s)",
                duration_ms=TIMEOUT_SECONDS * 1000
            )
        except Exception as e:
            return TestResult(
                name=test_name,
                path=test_path,
                passed=False,
                expected_to_fail=expected_to_fail,
                output="",
                error=str(e),
                duration_ms=0
            )

    def find_tests(self, category: Optional[str] = None, pattern: Optional[str] = None, test_file: Optional[str] = None) -> List[Path]:
        """Find all test files matching the filter criteria."""
        tests = []

        # If specific test file is provided, try to find it
        if test_file:
            # Try as absolute path first
            test_path = Path(test_file)
            if not test_path.exists():
                # Try relative to test directory
                test_path = self.test_dir / test_file
                if not test_path.exists():
                    # Try with .mmt extension
                    test_path = self.test_dir / f"{test_file}.mmt"
                    if not test_path.exists():
                        # Try finding by name pattern
                        for candidate in self.test_dir.rglob("*.mmt"):
                            if candidate.name == test_file or candidate.name == f"{test_file}.mmt":
                                return [candidate]
                        return []
            return [test_path]

        for test_file in self.test_dir.rglob("*.mmt"):
            relative_test_path = test_file.relative_to(self.test_dir)

            # Documentation examples are compiled through scripts/testing/doc_examples.py
            # because their extracted temp paths may differ from their tracked mirrors.
            if relative_test_path.parts and relative_test_path.parts[0] == "doc_examples":
                continue

            # Skip non-test files
            if test_file.name in ['minimal_test.mmt', 'test.mmt', 'simple_test.mmt', 'test_backup.mmt']:
                if test_file.parent == self.test_dir:
                    continue

            # Apply category filter
            if category:
                if category not in str(relative_test_path):
                    continue

            # Apply pattern filter
            if pattern:
                if pattern.lower() not in test_file.name.lower():
                    continue

            tests.append(test_file)

        return sorted(tests)

    def print_result(self, result: TestResult, show_output: bool = False):
        """Print a single test result."""
        status_icon = f"{Color.GREEN}[OK]{Color.RESET}" if result.passed else f"{Color.RED}[FAIL]{Color.RESET}"
        test_type = f"{Color.YELLOW}[SHOULD-FAIL]{Color.RESET}" if result.expected_to_fail else f"{Color.CYAN}[SUCCESS]{Color.RESET}"

        print(f"{status_icon} {test_type} {result.name} {Color.GRAY}({result.duration_ms:.0f}ms){Color.RESET}")

        if not result.passed and (self.verbose or show_output):
            print(f"  {Color.YELLOW}Exit code: {result.exit_code}{Color.RESET}")
            if result.error:
                print(f"  {Color.RED}Error: {result.error}{Color.RESET}")
            else:
                print(f"  {Color.GRAY}Output:{Color.RESET}")
                # Show all lines if single test, otherwise first 10 lines
                max_lines = None if show_output else 10
                for line in result.output.split('\n')[:max_lines]:
                    if line:  # Skip empty lines
                        print(f"    {Color.GRAY}{line}{Color.RESET}")

    def run_all_tests(self, category: Optional[str] = None, pattern: Optional[str] = None, test_file: Optional[str] = None):
        """Run all tests and print results."""
        tests = self.find_tests(category, pattern, test_file)

        if not tests:
            print(f"{Color.YELLOW}No tests found matching criteria{Color.RESET}")
            return 1

        # Auto-enable verbose output for single/few tests
        show_full_output = len(tests) <= 3

        print(f"{Color.BOLD}Marmot Test Suite{Color.RESET}")
        print(f"{Color.GRAY}{'=' * 60}{Color.RESET}")
        print(f"Executable: {Color.CYAN}{self.midori_exe}{Color.RESET}")
        print(f"Build: {Color.CYAN}{self.build_config}{Color.RESET}")
        print(f"Tests: {Color.CYAN}{len(tests)}{Color.RESET}")
        if show_full_output:
            print(f"Mode: {Color.CYAN}Detailed output enabled{Color.RESET}")
        print(f"{Color.GRAY}{'=' * 60}{Color.RESET}\n")

        # Group tests by category
        categories = {}
        for test in tests:
            category_name = str(test.relative_to(self.test_dir).parts[0])
            if category_name not in categories:
                categories[category_name] = []
            categories[category_name].append(test)

        # Run tests by category
        for cat_name in sorted(categories.keys()):
            print(f"\n{Color.BOLD}{Color.BLUE}[{cat_name}]{Color.RESET}")

            for test_path in categories[cat_name]:
                result = self.run_test(test_path)
                self.results.append(result)
                self.print_result(result, show_output=show_full_output)

        return self.print_summary()

    def print_summary(self) -> int:
        """Print test summary statistics."""
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed

        success_tests = [r for r in self.results if not r.expected_to_fail]
        failure_tests = [r for r in self.results if r.expected_to_fail]

        success_passed = sum(1 for r in success_tests if r.passed)
        failure_passed = sum(1 for r in failure_tests if r.passed)

        total_time = sum(r.duration_ms for r in self.results)

        print(f"\n{Color.GRAY}{'=' * 60}{Color.RESET}")
        print(f"{Color.BOLD}Test Summary{Color.RESET}\n")

        if failed == 0:
            print(f"{Color.GREEN}{Color.BOLD}[SUCCESS] All tests passed!{Color.RESET}")
        else:
            print(f"{Color.RED}{Color.BOLD}[FAILED] Some tests failed{Color.RESET}")

        print(f"\n{Color.CYAN}Total:{Color.RESET}     {passed}/{total} passed")
        print(f"{Color.CYAN}Success:{Color.RESET}   {success_passed}/{len(success_tests)} passed")
        print(f"{Color.CYAN}Failure:{Color.RESET}   {failure_passed}/{len(failure_tests)} passed (should fail)")
        print(f"{Color.CYAN}Duration:{Color.RESET}  {total_time:.0f}ms")

        if failed > 0:
            print(f"\n{Color.RED}Failed tests:{Color.RESET}")
            for result in self.results:
                if not result.passed:
                    print(f"  {Color.RED}[X]{Color.RESET} {result.name}")

        print(f"{Color.GRAY}{'=' * 60}{Color.RESET}")

        return 0 if failed == 0 else 1

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the Marmot language suite.")
    add_build_arguments(parser)
    parser.add_argument("--category", help="Only tests under this folder of test/ (e.g. closure)")
    parser.add_argument("--pattern", help="Only tests whose file name contains this")
    parser.add_argument("--test", help="One test (e.g. closure/simple.mmt or just simple)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show each failure's output")
    args = parser.parse_args(argv)

    runner = TestRunner(BuildTree.from_args(args), verbose=args.verbose)
    try:
        return runner.run_all_tests(category=args.category, pattern=args.pattern, test_file=args.test)
    finally:
        cleanup_language_test_artifacts(REPO_ROOT)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
