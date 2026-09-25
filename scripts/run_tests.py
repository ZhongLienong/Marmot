#!/usr/bin/env python3
"""
Marmot Test Runner

Runs all tests in the test/ directory and reports results.
Supports:
- Expected output verification (.expected files)
- Structured warning verification (.warnings.json files)
- Failure tests (tests in failure/ directories should fail compilation)
- Success tests (tests in success/ directories should succeed)
- Colored output with detailed reporting
- Test filtering by category, pattern, or specific test file
- Automatic detailed output for single/few tests

Usage:
    python scripts/run_tests.py                          # Run all tests
    python scripts/run_tests.py --category closure       # Run only closure tests
    python scripts/run_tests.py --pattern loop           # Run tests matching 'loop'
    python scripts/run_tests.py --test closure/simple    # Run specific test
    python scripts/run_tests.py --verbose                # Show detailed output
    python scripts/run_tests.py --build Debug            # Use Debug build
"""

import os
import sys
import subprocess
import argparse
import json
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional
import re
import tempfile

# Color codes
class Color:
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    MAGENTA = '\033[95m'
    CYAN = '\033[96m'
    WHITE = '\033[97m'
    GRAY = '\033[90m'
    BOLD = '\033[1m'
    RESET = '\033[0m'

def executable_name(stem: str) -> str:
    return stem + (".exe" if os.name == "nt" else "")


def preset_prefix() -> str:
    """The CMakePresets.json family for this host: x64-* on Windows, linux-* elsewhere."""
    return "x64-" if os.name == "nt" else "linux-"


def vm_beside(compiler: Path) -> Path:
    """marmotvm, built beside marmotc."""
    return compiler.with_name("marmotvm" + compiler.suffix)


def build_and_run(
    compiler: Path,
    source: str,
    *,
    cwd: Path,
    env: dict,
    timeout: float,
) -> subprocess.CompletedProcess:
    """Build `source` with marmotc and run it in marmotvm, as `marmot run` does.

    The result's output is the build's, then the run's; its exit code is the
    build's when the build failed. A timeout covers each step.
    """
    with tempfile.TemporaryDirectory(prefix="marmot-run-") as directory:
        program = Path(directory) / (Path(source).stem + ".mmc")
        options = dict(capture_output=True, text=True, encoding='utf-8', errors='replace',
                       timeout=timeout, cwd=cwd, env=env, check=False)
        built = subprocess.run([str(compiler), "build", source, "-o", str(program), "--quiet"], **options)
        if built.returncode != 0:
            return built
        ran = subprocess.run([str(vm_beside(compiler)), str(program)], **options)
        return subprocess.CompletedProcess(
            ran.args, ran.returncode, built.stdout + ran.stdout, built.stderr + ran.stderr)


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
    def __init__(self, build_config: str = "Development", verbose: bool = False):
        self.root_dir = Path(__file__).parent.parent
        self.test_dir = self.root_dir / "test"
        self.requested_build_config = build_config
        self.build_config = build_config
        self.verbose = verbose
        self.executable_notice: Optional[str] = None
        self.executable_search_errors: List[str] = []

        # Find the Marmot executable
        self.midori_exe = self.find_executable()
        if not self.midori_exe:
            print(f"{Color.RED}Error: Could not find Marmot executable{Color.RESET}")
            for message in self.executable_search_errors:
                print(f"{Color.YELLOW}  - {message}{Color.RESET}")
            sys.exit(1)

        # Test results
        self.results: List[TestResult] = []

    def find_executable(self) -> Optional[Path]:
        """Find the Marmot executable based on build configuration."""
        requested_candidates = self.get_executable_candidates(self.requested_build_config)
        requested_errors: List[str] = []

        for path in requested_candidates:
            if not path.exists():
                continue

            validation_error = self.validate_executable(path, self.requested_build_config)
            if validation_error is None:
                return path
            if validation_error.startswith("configured as "):
                self.executable_notice = (
                    f"Requested {self.requested_build_config} build tree reports a different CMake build type; "
                    f"using {path} anyway because the executable exists."
                )
                self.executable_search_errors = [f"{path} ({validation_error})"]
                return path
            requested_errors.append(f"{path} ({validation_error})")

        if requested_errors and self.requested_build_config != "Debug":
            debug_candidates = self.get_executable_candidates("Debug")
            for path in debug_candidates:
                if not path.exists():
                    continue

                validation_error = self.validate_executable(path, "Debug")
                if validation_error is None:
                    self.build_config = "Debug"
                    self.executable_notice = (
                        f"Requested {self.requested_build_config} build is unavailable or invalid; "
                        f"using Debug executable instead."
                    )
                    self.executable_search_errors = requested_errors
                    return path

        self.executable_search_errors = requested_errors

        return None

    def get_executable_candidates(self, build_config: str) -> List[Path]:
        preset = preset_prefix() + build_config.lower()
        return [
            self.root_dir / f"out/build/ninja/{preset}/out/{executable_name('marmotc')}",
            self.root_dir / f"out/build/{preset}/out/{executable_name('marmotc')}",
        ]

    def validate_executable(self, path: Path, expected_build_config: Optional[str] = None) -> Optional[str]:
        build_dir = path.parent.parent
        cache_path = build_dir / "CMakeCache.txt"
        if not cache_path.exists():
            return None

        try:
            cache_text = cache_path.read_text(encoding='utf-8', errors='replace')
        except OSError as exc:
            return f"could not read CMakeCache.txt: {exc}"

        build_match = re.search(r'^CMAKE_BUILD_TYPE:STRING=(.+)$', cache_text, re.MULTILINE)
        if expected_build_config is not None and build_match is not None:
            actual_build_config = build_match.group(1).strip()
            if actual_build_config.lower() != expected_build_config.lower():
                return f"configured as {actual_build_config}, expected {expected_build_config}"

        generator_match = re.search(r'^CMAKE_GENERATOR:INTERNAL=(.+)$', cache_text, re.MULTILINE)
        if generator_match is None:
            return None

        generator = generator_match.group(1).strip()
        if generator == "Ninja":
            missing_files: List[str] = []
            if not (build_dir / "build.ninja").exists():
                missing_files.append("build.ninja")
            if not (build_dir / "CMakeFiles" / "rules.ninja").exists():
                missing_files.append("CMakeFiles/rules.ninja")
            if missing_files:
                return f"Ninja build tree is incomplete (missing {', '.join(missing_files)})"
            return None

        if generator == "NMake Makefiles":
            if not (build_dir / "Makefile").exists():
                return "NMake build tree is incomplete (missing Makefile)"
            return None

        return None

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

    def run_test(self, test_path: Path) -> TestResult:
        """Run a single test file."""
        relative_path = test_path.relative_to(self.test_dir)
        test_name = str(relative_path)
        command_path = str(test_path.relative_to(self.root_dir))

        expected_to_fail = self.is_failure_test(test_path)
        expected_output = self.get_expected_output(test_path)

        try:
            expected_warnings = self.get_expected_warnings(test_path)
            import time
            start = time.time()
            env = os.environ.copy()
            env["MARMOT_TEST_MODE"] = "1"
            # This checkout's prelude first: an installed one may be older.
            env["MARMOT_PATH"] = os.pathsep.join(
                entry for entry in [str(self.root_dir / "MarmotPrelude"), os.environ.get("MARMOT_PATH", "")] if entry != ""
            )
            if expected_warnings is not None:
                env["MARMOT_TEST_WARNING_FORMAT"] = "machine"

            result = build_and_run(self.midori_exe, command_path, cwd=self.root_dir, env=env, timeout=30)

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
                error="Test timed out (30s)",
                duration_ms=30000
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

            # Documentation examples are compiled through scripts/check_doc_examples.py
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
            return

        # Auto-enable verbose output for single/few tests
        show_full_output = len(tests) <= 3

        print(f"{Color.BOLD}Marmot Test Suite{Color.RESET}")
        print(f"{Color.GRAY}{'=' * 60}{Color.RESET}")
        print(f"Executable: {Color.CYAN}{self.midori_exe}{Color.RESET}")
        print(f"Build: {Color.CYAN}{self.build_config}{Color.RESET}")
        if self.executable_notice:
            print(f"Notice: {Color.YELLOW}{self.executable_notice}{Color.RESET}")
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

        # Print summary
        self.print_summary()

    def print_summary(self):
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

        # Exit with appropriate code
        sys.exit(0 if failed == 0 else 1)

def main():
    parser = argparse.ArgumentParser(description='Run Marmot test suite')
    parser.add_argument('--build', default='Development',
                        choices=['Debug', 'Development', 'Release'],
                        help='Build configuration to use (default: Development)')
    parser.add_argument('--category', help='Run only tests in specified category (e.g., closure)')
    parser.add_argument('--pattern', help='Run only tests matching pattern')
    parser.add_argument('--test', help='Run specific test file (e.g., closure/simple.mmt or just simple)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show detailed output')

    args = parser.parse_args()

    runner = TestRunner(build_config=args.build, verbose=args.verbose)
    runner.run_all_tests(category=args.category, pattern=args.pattern, test_file=args.test)

if __name__ == '__main__':
    main()
