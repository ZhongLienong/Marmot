# Testing Guide

Marmot has two complementary test layers:

- `common/tests/`, `runtime/tests/` and `compiler/tests/` contain in-process implementation tests built with Catch2 and linked against the Marmot libraries.
- `test/` contains file-based language regression tests run through `Marmot.exe test` and the legacy Python runners.

Use the smallest layer that proves the behavior you are changing. If a regression is important at both the subsystem and CLI level, add both.

## Choose the Right Test Layer

Add a test under `tests/` when the behavior is best validated in-process:

- lexer, parser, type checker, static analyzer, module graph, or VM behavior
- assertions can target tokens, AST shape, warnings, errors, exit codes, or captured output
- the fixture can be created inline or with `TempDir` / `TempProject`
- a failing test should point at a specific implementation seam instead of a black-box executable run

Add a test under `test/` when the behavior is best validated as a user-visible program run:

- the scenario naturally lives as one or more `.mmt` files on disk
- the regression depends on the executable boundary, startup behavior, or file layout
- the assertion is exit status, `.expected` output, or `.warnings.json`
- the test should exercise the same path that `Marmot.exe` users take

Default rule:

- if the subsystem is directly reachable through the Marmot libraries or `compiler/tests/support`, prefer a C++ unit test in the component's `tests/` folder
- if the value comes from a full-program fixture and black-box execution, prefer `test/`

## Layout and Conventions

Implementation tests:

- place files under the owning component's `tests/<area>/` folder: `common/tests/`, `runtime/tests/` or `compiler/tests/`
- name files `<Subsystem>Tests.cpp`
- use behavior-focused `TEST_CASE` names
- tag by area first, then by narrower slice when useful, for example `[module][import]` or `[runtime][vm][error]`
- keep each `TEST_CASE` focused on one regression or semantic rule

Regression tests:

- place programs under `test/<category>/`
- use `failure/` directories for compile-fail scenarios
- add `<name>.expected` when stdout/stderr or compile-fail diagnostics must match a snapshot
- `.expected` snapshots are compared after stripping ANSI color codes and repo-root path prefixes
- add `<name>.warnings.json` when warnings need structured assertions
- when a `.warnings.json` file is present, `Marmot.exe test` compares the emitted warning JSON against that snapshot
- `Marmot.exe test` enforces `[test].timeout_ms` by running each fixture in an isolated worker process
- `scripts/run_tests.py` still supports the legacy `MARMOT_TEST_WARNING_FORMAT=machine` path during the transition
- CLI contract checks that do not fit the plain `test/<category>/*.mmt` model run through `scripts/check_cli_contracts.py`

`<name>.warnings.json` fixtures use one ordered JSON array whose entries mirror the machine-readable warning objects emitted by the CLI diagnostic schema:

```json
[
  {
    "source": "marmot",
    "severity": "warning",
    "stage": "StaticAnalyzer",
    "code": "UnusedLocal",
    "file": "test/static_analyzer/success/unused_local_warning.mmt",
    "file_path": "test/static_analyzer/success/unused_local_warning.mmt",
    "line": 4,
    "column": 4,
    "endLine": 4,
    "endColumn": 10,
    "caret_length": 6,
    "message": "Binding 'unused' is never read.",
    "suggestion": null,
    "relatedInformation": []
  }
]
```

Keep the object order stable when warning order matters; the runner compares the full decoded array, not a set.

Documentation examples:

- runnable Markdown examples use fenced blocks whose info string starts with `marmot-test`
- `scripts/check_doc_examples.py` extracts those fences, verifies their mirrors under `test/doc_examples/`, and compiles them from repo-aware temporary paths
- the tracked mirrors under `test/doc_examples/` are sync targets for review; `scripts/run_tests.py` does not execute them directly
- `python scripts/check_doc_examples.py --sync` rewrites current mirrors and removes orphaned mirror artifacts that no longer correspond to any `marmot-test` fence
- use `name=<category>/<example>` for the stable mirror path under `test/doc_examples/<kind>/`
- use `path=<repo-relative-temp-file>` for the actual extraction target used during compilation
- use `module=<ModuleName>` when the snippet intentionally omits the required `module` declaration
- use `kind=failure` for documented examples that are expected to fail compilation

## Support Helpers

The helpers in `compiler/tests/support/` exist to keep new tests short and deterministic.

`CompileHelpers` in [`compiler/tests/support/CompileHelpers.h`](../compiler/tests/support/CompileHelpers.h):

- `LexSnippet(source, file_name)` returns `std::expected<LexedSnippet, CompilerError>`
- `ParseSnippet(source, file_name)` returns parsed statements, module declaration metadata, and collected `use` imports
- `TypeCheckSnippet(source, file_name)` returns the typed program tree and warnings
- `AnalyzeSnippet(source, file_name)` returns warnings and static-analyzer errors
- `CompileSnippetWithReport(source, file_name)` returns the final `MidoriResult::CompilationResult` from the driver boundary
- `CompilationReport(result)` returns the final `MidoriResult::CompilerReport` for either a successful or failed compile result
- `CompileSnippet(source, file_name)` compiles through the driver layer without launching the CLI
- `ExecuteSnippet(source, file_name)` compiles and runs a snippet in-process and captures stdout/stderr
- `CollectTokenNames(tokens)` turns a token stream into a concise sequence for lexer assertions

`CompileSnippet` and `ExecuteSnippet` already force Marmot test mode, so most unit tests do not need to set `MARMOT_TEST_MODE` manually.

Filesystem and environment helpers:

- [`compiler/tests/support/TempDir.h`](../compiler/tests/support/TempDir.h) creates an isolated temporary directory and removes it on scope exit
- [`compiler/tests/support/TempProject.h`](../compiler/tests/support/TempProject.h) builds small module trees for import and build-graph tests
- [`compiler/tests/support/ScopedEnvVar.h`](../compiler/tests/support/ScopedEnvVar.h) sets and restores environment variables such as `MARMOT_PATH`
- [`compiler/tests/support/OutputCapture.h`](../compiler/tests/support/OutputCapture.h) captures native stdout/stderr when a test cannot use `ExecuteSnippet`

Diagnostic helpers:

- [`compiler/tests/support/DiagnosticMatchers.h`](../compiler/tests/support/DiagnosticMatchers.h) matches warnings and errors by stage, code, line, and message fragments
- `FindWarning(...)` and `FindError(...)` work on raw vectors, diagnostic collections, and top-level compiler reports
- prefer these matchers over exact full-render snapshots when only part of the diagnostic matters

Example patterns:

```cpp
const std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result =
	MidoriTest::LexSnippet("def value = 1;\n", "Value.mmt");

REQUIRE(lex_result.has_value());
REQUIRE(MidoriTest::CollectTokenNames(lex_result->m_tokens) == std::vector<Token::Name>
{
	Token::Name::DEF,
	Token::Name::IDENTIFIER_LITERAL,
	Token::Name::SINGLE_EQUAL,
	Token::Name::INTEGER_LITERAL,
	Token::Name::SINGLE_SEMICOLON
});
```

```cpp
const MidoriTest::TempProject project
({
	MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"./Lib.mmt\" }\ndef main = fn() -> Int => 0;\n"),
	MidoriTest::TempProjectFile("Lib.mmt", "module Lib\ndef value = 1;\n")
});
```

```cpp
const CompilerWarning* warning = MidoriTest::FindWarning(analyze_result->m_warnings, CompilerWarningCode::UnusedLocal);
REQUIRE(warning != nullptr);

std::string mismatch;
REQUIRE(MidoriTest::Matches(
	*warning,
	MidoriTest::WarningExpectation
	{
		.m_stage = CompilerStage::StaticAnalyzer,
		.m_line = 4,
		.m_message_substrings = { "never read" }
	},
	&mismatch));
```

## Command Matrix

One-command entry point from the repo root:

```powershell
python scripts/test_project.py
python scripts/test_project.py --mode unit --build Debug
python scripts/test_project.py --mode unit --unit-tag "[runtime]"
python scripts/test_project.py --mode regression --category closure
python scripts/test_project.py --mode regression --category doc_examples
python scripts/test_project.py --mode regression --category cli_contracts
```

Run the runnable Markdown examples directly:

```powershell
python scripts/check_doc_examples.py --build Development
python scripts/check_doc_examples.py --build Development --sync
```

`python scripts/test_project.py --mode regression` runs the doc-example check automatically before the file-based regression suite unless you pass `--skip-doc-examples`.

Run the CLI contract checks directly:

```powershell
python scripts/check_cli_contracts.py --build Development
```

`python scripts/test_project.py --mode regression` also runs `scripts/check_cli_contracts.py` unless you pass `--skip-cli-contracts`.

Run the formatter idempotency check directly:

```powershell
python scripts/check_format.py --build Development
python scripts/check_format.py --build Development --root test --root MarmotPrelude
python scripts/check_format.py --build Development --enforce-clean
```

`python scripts/test_project.py --mode regression` also runs `scripts/check_format.py` unless you pass `--skip-format-check`. The check verifies that
`marmot fmt` is idempotent across the test corpus, the prelude, and the
reference package. The optional `--enforce-clean` flag additionally requires
`marmot fmt --check` to pass on each scanned root.

Compile every benchmark program, and optionally run them:

```powershell
python scripts/check_benchmarks.py --build Development
python scripts/check_benchmarks.py --build Release --run
```

The programs under `benchmarks/` print timings, so they have no snapshots and
are not part of the regression suite. `scripts/check_benchmarks.py` runs
`marmot check` on each one and fails on any compile error or warning, so a
language change cannot leave them uncompilable unnoticed (it did once: every
benchmark stopped compiling when v2 removed `loop`, assignment and in-place
`Appendable`). `--run` also executes each one; its timings are only meaningful
with a Release build. `python scripts/test_project.py --mode regression` runs
the compile check unless you pass `--skip-benchmark-check`, and
`--category benchmarks` runs it alone.

Check that the components keep their dependency direction:

```powershell
python scripts/check_layering.py
```

The C++ code builds as four libraries: `MarmotCommon` (`common/src`),
`MarmotRuntime` (`runtime/src`, links Common), `MarmotCompiler`
(`compiler/src`, links Common, never Runtime) and `MarmotDriver` (the CLI,
driver and test runner under `compiler/src/Utility`, links Compiler and
Runtime). Each library exports only its own include root, so most wrong
includes already fail to compile; the script also catches the ones a shared
include path would let through, such as compiler code including a driver
header. It also keeps the compiler pipeline (`compiler/src/Compiler`, apart
from the package manager) free of project discovery: those files may not
include the package manager or `Utility/Project`, or read environment
variables, because the compiler compiles from the `CompilationInputs` its
caller passes. It needs no build, and `python scripts/test_project.py` runs it
first unless you pass `--skip-layering-check`.

Configure and build implementation tests on Windows:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --target MarmotUnitTests

cmake --preset x64-development
cmake --build --preset x64-development --target MarmotUnitTests
```

Configure and build implementation tests on Linux:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target MarmotUnitTests
```

Release builds leave `MIDORI_BUILD_TESTS` off by default. Opt in explicitly when needed:

```powershell
cmake --preset x64-release -DMIDORI_BUILD_TESTS=ON
cmake --build --preset x64-release --target MarmotUnitTests
```

Run all registered Catch2 suites through CTest:

```powershell
ctest --test-dir out/build/ninja/x64-debug --output-on-failure
ctest --test-dir out/build/ninja/x64-development --output-on-failure
```

```bash
ctest --test-dir out/build/ninja/linux-debug --output-on-failure
```

Filter implementation tests by discovered test name. For area tags such as `[runtime]`, running the Catch2 executable directly is usually simpler:

```powershell
ctest --test-dir out/build/ninja/x64-debug --output-on-failure -R TypeChecker
ctest --test-dir out/build/ninja/x64-debug --output-on-failure -R ImportResolver
```

Run the Catch2 executable directly when you want tag filtering:

```powershell
.\out\build\ninja\x64-debug\out\MarmotUnitTests.exe [runtime]
.\out\build\ninja\x64-debug\out\MarmotUnitTests.exe [module][import]
```

```bash
./out/build/ninja/linux-debug/out/MarmotUnitTests [runtime]
```

Run the file-based regression suite through the native CLI:

```powershell
.\out\build\ninja\x64-development\out\Marmot.exe test
.\out\build\ninja\x64-development\out\Marmot.exe test closure
.\out\build\ninja\x64-development\out\Marmot.exe test --pattern recursive
```

Legacy Python runner:

```powershell
python scripts/run_tests.py --build Development
python scripts/run_tests.py --build Debug
```

The native command does not replace `scripts/run_tests.py` yet.
The Python runner remains useful for existing developer workflows and for
cross-checking CLI behavior during the transition.
For full regression runs without filters, `scripts/test_project.py` also runs `scripts/check_doc_examples.py` before `scripts/run_tests.py`.
For full regression runs without filters, `scripts/test_project.py` also runs `scripts/check_cli_contracts.py`.

Filter regression tests:

```powershell
python scripts/run_tests.py --category closure --build Development
python scripts/run_tests.py --category static_analyzer --build Development
python scripts/run_tests.py --pattern recursive --build Development
python scripts/run_tests.py --test closure/simple.mmt --build Development
```

## Authoring Checklist

- choose `tests/` unless the regression specifically needs black-box executable coverage
- use the narrowest helper that reaches the subsystem you are changing
- prefer structured assertions over full rendered-output snapshots
- keep temp filesystem state inside `TempDir` or `TempProject`
- keep environment changes scoped with `ScopedEnvVar`
- add a `test/` fixture in addition to a unit test when the behavior is critical at the language level
