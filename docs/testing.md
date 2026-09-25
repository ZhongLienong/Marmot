# Testing Guide

Marmot has two complementary test layers:

- `common/tests/`, `runtime/tests/` and `compiler/tests/` contain in-process implementation tests built with Catch2 and linked against the Marmot libraries.
- `test/` contains file-based language regression tests run through `marmot test` and the legacy Python runners.

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
- the test should exercise the same path that users take: `marmotc` builds it and `marmotvm` runs it

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
- when a `.warnings.json` file is present, `marmot test` compares the emitted warning JSON against that snapshot
- `marmot test` builds each fixture with `marmotc` into `target/` and runs it in its own `marmotvm` process, in parallel, enforcing `[test].timeout_ms` on each
- `python scripts/dev.py test` runs the same suite through `scripts/testing/language.py`, which asks for warnings through `MARMOT_TEST_WARNING_FORMAT=machine`
- `python scripts/dev.py snapshot <test>` writes a test's `.expected` (and `.warnings.json`, when it has one or with `--warnings`) from its current output; `--force` rewrites existing ones. Read the diff before committing: a snapshot rewritten to match a regression hides it
- CLI contract checks that do not fit the plain `test/<category>/*.mmt` model run through `scripts/testing/cli_contracts.py`

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
- `scripts/testing/doc_examples.py` extracts those fences, verifies their mirrors under `test/doc_examples/`, and compiles them from repo-aware temporary paths
- the tracked mirrors under `test/doc_examples/` are sync targets for review; the language suite does not execute them directly
- `python scripts/dev.py check docs --sync` rewrites current mirrors and removes orphaned mirror artifacts that no longer correspond to any `marmot-test` fence
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

`scripts/dev.py` runs every check, on Windows and Linux alike, and builds what
a check needs first (`--no-build` skips that). `--build` picks the
configuration, Development by default.

The full gate, stopping at the first failure:

```bash
python scripts/dev.py gate
python scripts/dev.py gate --build Release
python scripts/dev.py gate --skip unit tool
python scripts/dev.py gate --only docs cli format
```

Its steps, in order, are `layering`, `build`, `unit`, `docs`, `cli`, `format`,
`benchmarks`, `tool` and `language`. Each check also runs alone:

```bash
python scripts/dev.py check layering
python scripts/dev.py check docs              # --sync rewrites the mirrors
python scripts/dev.py check cli
python scripts/dev.py check format            # --root DIR, --enforce-clean
python scripts/dev.py check benchmarks        # --run executes them too
python scripts/dev.py check tool              # the marmot tool's cargo tests
```

The format check verifies that `marmotc fmt` is idempotent across the test
corpus and the prelude. The optional `--enforce-clean` flag additionally
requires `marmotc fmt --check` to pass on each scanned root.

The programs under `benchmarks/` print timings, so they have no snapshots and
are not part of the regression suite. The benchmarks check runs `marmot check`
on each one and fails on any compile error or warning, so a language change
cannot leave them uncompilable unnoticed (it did once: every benchmark stopped
compiling when v2 removed `loop`, assignment and in-place `Appendable`). `--run`
also executes each one; its timings are only meaningful with a Release build,
and `python scripts/dev.py bench` measures them properly.

The layering check keeps the components' dependency direction.
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
caller passes. It needs no build, and the gate runs it first.

Run the implementation tests:

```bash
python scripts/dev.py unit                        # every suite, through CTest
python scripts/dev.py unit --regex TypeChecker    # CTest names
python scripts/dev.py unit --tag "[runtime]"      # Catch2 tags
python scripts/dev.py unit --tag "[module][import]" --build Debug
```

Release builds leave `MIDORI_BUILD_TESTS` off by default;
`python scripts/dev.py configure --build Release --unit-tests` opts in, and the
Release gate does that itself. By hand, the presets are `x64-*` on Windows and
`linux-*` on Linux:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target MarmotUnitTests
ctest --test-dir out/build/ninja/linux-debug --output-on-failure
./out/build/ninja/linux-debug/out/MarmotUnitTests [runtime]
```

Run the file-based regression suite:

```bash
python scripts/dev.py test
python scripts/dev.py test --category closure
python scripts/dev.py test --category static_analyzer --build Debug
python scripts/dev.py test --pattern recursive
python scripts/dev.py test --test closure/simple.mmt --verbose
```

The marmot tool runs it too, from the repository root, with `MARMOTC` naming
the compiler and `MARMOT_PATH` the prelude:

```powershell
$env:MARMOTC = ".\out\build\ninja\x64-development\out\marmotc.exe"
$env:MARMOT_PATH = "$PWD\MarmotPrelude"
marmot test
marmot test closure
marmot test --pattern recursive
```

## Authoring Checklist

- choose `tests/` unless the regression specifically needs black-box executable coverage
- use the narrowest helper that reaches the subsystem you are changing
- prefer structured assertions over full rendered-output snapshots
- keep temp filesystem state inside `TempDir` or `TempProject`
- keep environment changes scoped with `ScopedEnvVar`
- add a `test/` fixture in addition to a unit test when the behavior is critical at the language level
