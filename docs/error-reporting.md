# Error Reporting

Marmot's diagnostic pipeline is structured. Compiler stages produce
`CompilerError` and `CompilerWarning` values, runtime execution produces
`RuntimeError`, and rendering is deferred until the driver or CLI boundary.

## Core Types

Source:

- `common/src/Common/Error/Error.h`
- `common/src/Common/Error/Error.cpp`
- `compiler/src/Compiler/Result/Result.h`
- `compiler/src/Utility/Driver/MidoriDriver.cpp`

Main types:

- `CompilerError`
- `CompilerWarning`
- `CompilerErrorLocation`
- `RuntimeError`
- `RuntimeErrorCode`
- `RuntimeDiagnosticKind`
- `RuntimeStackFrame`
- `MidoriResult::CompilerWarnings`
- `MidoriResult::CompilerDiagnostics`
- `MidoriResult::CompilerReport`

## Stages

Diagnostics carry a `CompilerStage`:

- `Lexer`
- `Parser`
- `TypeChecker`
- `StaticAnalyzer`
- `CodeGenerator`
- `Module`
- `Optimizer`
- `BytecodeLinker`
- `Compiler`
- `Runtime`
- `Unknown`

These stage names are also used in machine-readable output.

## Codes

### Compiler Error Codes

Current `CompilerErrorCode` values:

- `None`
- `NoMatch`
- `ModuleImportResolutionFailed`
- `ModuleImportFileOpenFailed`
- `ModuleCircularDependency`
- `ModuleDeclarationMissing`
- `ModuleDeclarationDuplicate`
- `ModuleMissingExportedSymbol`
- `TypeUndefinedName`
- `TypeUnsatisfiedConstraint`
- `TypeNotCallable`
- `TypeIncorrectArity`
- `TypeMismatch`
- `TypeNonExhaustiveMatch`
- `CodeGeneratorLimitExceeded`
- `CodeGeneratorUnresolvedMethodResolution`
- `CodeGeneratorAmbiguousMethodResolution`
- `CodeGeneratorUnsupportedLowering`
- `CodeGeneratorUnknownForeignFunction`
- `BytecodeLinkerNoModulesToLink`
- `BytecodeLinkerDuplicateExportedSymbol`
- `BytecodeLinkerUnresolvedImport`
- `CompilerNoModulesReadyToCompile`
- `CompilerIncompleteCompilationSchedule`
- `CompilerMissingCompiledModule`

### Warning Codes

Current `CompilerWarningCode` values:

- `None`
- `NameShadowing`
- `UnusedLocal`
- `IntegerOverflow`
- `CellCrossesWorker`

### Runtime Error Codes

Current `RuntimeErrorCode` values:

- `None`
- `IndexOutOfBounds`
- `NegativeArraySize`
- `ArraySizeExceeded`
- `FFIFunctionNotFound`
- `StackOverflow`
- `MemoryAccessViolation`
- `DivisionByZero`
- `InternalTypeError`
- `WorkerCancelled`
- `WorkerExited`
- `InvalidConversion`
- `InternalFFITypeError`

`RuntimeErrorCodeName(...)` provides the stable string form used in rendered and
machine-readable output.

## Location Model

`CompilerErrorLocation` can carry:

- `file_name`
- `line`
- `column`
- `caret_length`
- `end_line`
- `end_column`
- `source_line`

Not every diagnostic includes all fields:

- `WithToken(...)` derives line, column, caret length, and source line from a
  `Token`
- `WithContext(...)` accepts them explicitly
- `Simple(...)` creates an unlocated diagnostic that renders as a plain message
- runtime diagnostics reuse the same location type so compile-time and runtime
  JSON stay aligned

## Human-Readable Rendering

Compiler diagnostics render with:

- a stage/severity header
- file and line
- source line context
- caret highlighting when column info exists
- an optional suggestion line

Example compiler shape:

```text
Parser Error at Format.mmt:2
  |
2 | def value = ;
  |     ^^^ Expected expression
  |
  | Try adding a literal
```

Runtime diagnostics render from `RuntimeError`:

- recoverable runtime failures render as `error[Code]: ...`
- panics render as `panic[Code]: ...`
- stack traces render from structured `RuntimeStackFrame` data
- embedded source lines are used when available, with file reads as fallback on
  the error path

Example runtime shape:

```text
error[IndexOutOfBounds]: Index out of bounds at index: 4.
 --> Runtime.mmt:2
  |
2 | def value = [1, 2][4];
  | Index out of bounds at index: 4.
  |
stack trace:
  at main [module Runtime] in Runtime.mmt:3
```

Exit codes:

- runtime `error`: `1`
- runtime `panic`: `2`

## Report Aggregation

`CompilerReport` combines warnings and errors in one object:

- warnings are stored in `CompilerWarnings`
- errors are stored in `CompilerDiagnostics`
- `RenderedWarnings()` renders grouped warning summaries followed by the warning
  bodies
- `RenderedErrors()` renders the error bodies
- `Rendered()` concatenates warnings first, then errors

Important behavior from the current pipeline:

- successful compilations can still return warnings
- warnings from earlier stages survive later failures
- warnings are not printed immediately when produced
- the driver prints warnings once, in build-schedule order

That ordering is tested in:

- `common/tests/common/ErrorFormattingTests.cpp`
- `compiler/tests/compiler/CompilerWarningAggregationTests.cpp`

## Driver Behavior

`MidoriDriver` uses the report/runtime types like this:

- `CompileSourceWithReport(...)` preserves warnings and errors together
- `RunExecutable(...)` returns `std::expected<int, RuntimeError>`
- on successful compile-and-run, warnings are emitted before execution starts
- on compilation failure, warnings are rendered before the final errors
- the `"Compilation failed :( "` banner is only used for compilation failures
- runtime diagnostics are rendered without the compilation banner

## Machine-Readable Output

There are three relevant machine-readable surfaces.

### Line-Oriented Warning Output

`SerializeMachineReadableWarning(...)` emits:

```text
MARMOT_WARNING\t{"stage":"StaticAnalyzer", ...}
```

This path exists mainly for legacy test harnesses that still want warnings as a
line stream.

### Compiler JSON Payloads

The stable schema is documented in [Diagnostic Format](diagnostic-format.md).
Compiler diagnostics populate:

- `report.diagnostics`
- `report.warnings`
- `report.errors`

APIs:

- `SerializeMachineReadableError(...)`
- `SerializeMachineReadableWarningPayload(...)`
- `MidoriResult::CompilerWarnings::MachineReadable()`
- `MidoriResult::CompilerWarnings::MachineReadableJson()`
- `MidoriResult::CompilerDiagnostics::MachineReadableJson()`
- `MidoriResult::CompilerReport::MachineReadableJson()`

### Runtime JSON Payloads

Runtime failures serialize with:

- `SerializeMachineReadableRuntimeError(...)`

Runtime JSON extends the compiler schema with:

- `source: "marmot-runtime"`
- `kind`
- `sourceLine`
- `exitCode`
- `stack`

`marmot run --format json` merges the optional runtime diagnostic into the same
report envelope used by compiler diagnostics. The runtime failure is appended to
`report.diagnostics` and `report.errors`.

## Test Harness Toggle

If `MARMOT_TEST_WARNING_FORMAT=machine` is present in the environment, the
driver prints machine-readable warning lines alongside the normal
human-readable warning output.

This is deprecated in favor of `--format json`, but remains available for the
legacy Python test runner.

## Practical Guidance

When adding new diagnostics:

- prefer a specific `CompilerStage`
- assign a stable error or warning code when the diagnostic should be
  regression-tested
- use `WithToken(...)` whenever a token span exists
- use `WithContext(...)` when the location is known but not anchored to one
  token
- keep rendering and machine-readable serialization derived from the structured
  diagnostic, not from ad hoc print statements
- for runtime failures, populate `RuntimeError` and let callers decide whether
  to render text or serialize JSON
