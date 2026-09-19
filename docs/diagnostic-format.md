# Diagnostic Format

Marmot's machine-readable diagnostic contract is exposed through `--format json`
on CLI commands such as:

```powershell
marmotc.exe check src/Main.mmt --format json
marmotc.exe build src/Main.mmt --format json
marmotvm.exe src/Main.mmc --format json
marmot run src/Main.mmt --format json
```

## Top-Level Envelope

`check`, `build` and `run` return a command envelope with a nested `report`
(`marmotvm` sets `"source": "marmotvm"`):

```json
{
  "version": 1,
  "source": "marmot",
  "command": "check",
  "success": true,
  "exitCode": 0,
  "stdout": "",
  "stderr": "",
  "report": {
    "version": 1,
    "source": "marmot",
    "diagnostics": [],
    "warnings": [],
    "errors": []
  }
}
```

Stable fields:

- `version`: schema version. Current value is `1`.
- `source`: always `"marmot"`.
- `command`: CLI command name.
- `success`: `true` when the command succeeded.
- `exitCode`: CLI exit code.
- `stdout`: captured command stdout.
- `stderr`: captured command stderr.
- `report`: compiler/runtime diagnostics for the command.

`marmotvm --format json` uses the same envelope. If execution fails at runtime,
the runtime diagnostic is in `report.diagnostics` and `report.errors`.
`marmot run --format json` merges the two: the build's warnings stay in
`report.warnings`, and the run's output and errors follow.

## Compiler Diagnostic Object

Compiler diagnostics in `report.diagnostics`, `report.warnings`, and
`report.errors` use this shape:

```json
{
  "source": "marmot",
  "severity": "warning",
  "stage": "StaticAnalyzer",
  "code": "UnusedLocal",
  "message": "Binding 'unused' is never read.",
  "file": "test/static_analyzer/success/unused_local_warning.mmt",
  "file_path": "test/static_analyzer/success/unused_local_warning.mmt",
  "line": 4,
  "column": 4,
  "endLine": 4,
  "endColumn": 10,
  "caret_length": 6,
  "suggestion": "Prefix with '_' if intentional.",
  "relatedInformation": []
}
```

Stable fields:

- `source`
- `severity`
- `stage`
- `code`
- `message`
- `file`
- `line`
- `column`
- `endLine`
- `endColumn`
- `suggestion`

Compatibility alias:

- `file_path`: legacy alias for `file`
- `caret_length`: retained for existing tooling; editors should prefer
  `endLine` / `endColumn`

`relatedInformation` is reserved for secondary spans such as "defined here". It
is currently emitted as an empty array when no related locations exist.

## Runtime Diagnostic Object

Runtime failures produced by `marmotvm --format json` use the same report envelope,
but the diagnostic object includes runtime-specific fields:

```json
{
  "source": "marmot-runtime",
  "severity": "error",
  "stage": "Runtime",
  "code": "StackOverflow",
  "kind": "panic",
  "message": "Stack overflow - exceeded maximum call depth.",
  "file": "Runtime.mmt",
  "file_path": "Runtime.mmt",
  "line": 5,
  "column": null,
  "endLine": 5,
  "endColumn": null,
  "caret_length": null,
  "sourceLine": "def value = recurse(0);",
  "exitCode": 2,
  "stack": [
    {
      "procedure": "recurse",
      "module": "Runtime",
      "file": "Runtime.mmt",
      "file_path": "Runtime.mmt",
      "line": 2,
      "column": null,
      "endLine": 2,
      "endColumn": null,
      "sourceLine": "def recurse = fn(n : Int) -> Int => recurse(n + 1) + 1;",
      "recursiveCount": 12
    }
  ]
}
```

Runtime-specific fields:

- `source`: always `"marmot-runtime"` for runtime diagnostics
- `kind`: `"error"` or `"panic"`
- `sourceLine`: embedded or file-backed source text for the primary location
- `exitCode`: `1` for recoverable runtime errors, `2` for panics
- `stack`: rendered stack trace data as structured frame objects

Runtime stack frame fields:

- `procedure`
- `module`
- `file`
- `file_path`
- `line`
- `column`
- `endLine`
- `endColumn`
- `sourceLine`
- `recursiveCount`

## Stages

Current `stage` values:

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

## Error Codes

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
- `InternalFFITypeError`

## Warning Codes

Current `CompilerWarningCode` values:

- `None`
- `NameShadowing`
- `UnusedLocal`
- `CaptureEscape`
- `IntegerOverflow`

## Examples by Severity

### Warning

```json
{
  "source": "marmot",
  "severity": "warning",
  "stage": "StaticAnalyzer",
  "code": "UnusedLocal",
  "message": "Binding 'unused' is never read.",
  "file": "test/static_analyzer/success/unused_local_warning.mmt",
  "file_path": "test/static_analyzer/success/unused_local_warning.mmt",
  "line": 4,
  "column": 4,
  "endLine": 4,
  "endColumn": 10,
  "caret_length": 6,
  "suggestion": "Prefix with '_' if intentional.",
  "relatedInformation": []
}
```

### Error

```json
{
  "source": "marmot",
  "severity": "error",
  "stage": "Parser",
  "code": "None",
  "message": "Expected expression.",
  "file": "test/expression/failure/missing_rhs.mmt",
  "file_path": "test/expression/failure/missing_rhs.mmt",
  "line": 2,
  "column": 12,
  "endLine": 2,
  "endColumn": 13,
  "caret_length": 1,
  "suggestion": null,
  "relatedInformation": []
}
```

### Runtime panic

```json
{
  "source": "marmot-runtime",
  "severity": "error",
  "stage": "Runtime",
  "code": "StackOverflow",
  "kind": "panic",
  "message": "Stack overflow - exceeded maximum call depth.",
  "file": "Runtime.mmt",
  "file_path": "Runtime.mmt",
  "line": 5,
  "column": null,
  "endLine": 5,
  "endColumn": null,
  "caret_length": null,
  "sourceLine": "def value = recurse(0);",
  "exitCode": 2,
  "stack": []
}
```

## Examples by Code

The examples below are condensed: they show the `code`, `stage`, `severity`,
and `message` fields. All other fields (`source`, `file`, `line`, `column`,
`endLine`, `endColumn`, `caret_length`, `suggestion`, `relatedInformation`)
are emitted with the same shape as the full examples above.

### Compiler warning codes

| Code | Stage | Example message |
|------|-------|-----------------|
| `NameShadowing` | `StaticAnalyzer` | `Binding 'count' shadows an earlier definition.` |
| `UnusedLocal` | `StaticAnalyzer` | `Binding 'unused' is never read.` |
| `CaptureEscape` | `StaticAnalyzer` | `Captured local 'state' escapes its defining scope.` |
| `IntegerOverflow` | `StaticAnalyzer` | `Integer literal '9999999999' overflows 'Int'.` |

### Compiler error codes

| Code | Stage | Example message |
|------|-------|-----------------|
| `NoMatch` | `Parser` | `No grammar rule matched the input.` |
| `ModuleImportResolutionFailed` | `Module` | `Could not resolve import '<Foo>'.` |
| `ModuleImportFileOpenFailed` | `Module` | `Could not open imported module file 'Foo.mmt'.` |
| `ModuleCircularDependency` | `Module` | `Circular dependency detected between 'A' and 'B'.` |
| `ModuleDeclarationMissing` | `Module` | `File does not declare a 'module' statement.` |
| `ModuleDeclarationDuplicate` | `Module` | `Duplicate 'module' declaration in file.` |
| `ModuleMissingExportedSymbol` | `Module` | `Module 'A' does not export symbol 'foo'.` |
| `TypeUndefinedName` | `TypeChecker` | `Undefined name 'frobnicate'.` |
| `TypeUnsatisfiedConstraint` | `TypeChecker` | `Type 'T' does not satisfy constraint 'Eq T'.` |
| `TypeNotCallable` | `TypeChecker` | `Value of type 'Int' is not callable.` |
| `TypeIncorrectArity` | `TypeChecker` | `Function expects 2 argument(s) but received 3.` |
| `TypeMismatch` | `TypeChecker` | `Expected 'Int' but got 'Text'.` |
| `TypeNonExhaustiveMatch` | `TypeChecker` | `Non-exhaustive 'match'; missing case for 'None'.` |
| `CodeGeneratorLimitExceeded` | `CodeGenerator` | `Function exceeds the maximum local count.` |
| `CodeGeneratorUnresolvedMethodResolution` | `CodeGenerator` | `Could not resolve method 'show' for type 'T'.` |
| `CodeGeneratorAmbiguousMethodResolution` | `CodeGenerator` | `Ambiguous method 'show' for type 'T'.` |
| `CodeGeneratorUnsupportedLowering` | `CodeGenerator` | `Cannot lower expression to bytecode.` |
| `CodeGeneratorUnknownForeignFunction` | `CodeGenerator` | `Unknown foreign function 'MIDORI_FFI_Foo': it is not a Marmot builtin. Name the library that exports it: foreign "MIDORI_FFI_Foo" ... from "library";` |
| `BytecodeLinkerNoModulesToLink` | `BytecodeLinker` | `No modules to link.` |
| `BytecodeLinkerDuplicateExportedSymbol` | `BytecodeLinker` | `Duplicate exported symbol 'main' in 'A' and 'B'.` |
| `BytecodeLinkerUnresolvedImport` | `BytecodeLinker` | `Unresolved import 'foo' from module 'A'.` |
| `CompilerNoModulesReadyToCompile` | `Compiler` | `No modules ready to compile.` |
| `CompilerIncompleteCompilationSchedule` | `Compiler` | `Compilation schedule is incomplete.` |
| `CompilerMissingCompiledModule` | `Compiler` | `Missing compiled module 'A'.` |

### Runtime error codes

| Code | Stage | Kind | Example message |
|------|-------|------|-----------------|
| `IndexOutOfBounds` | `Runtime` | `error` | `Array index 5 out of bounds for length 3.` |
| `NegativeArraySize` | `Runtime` | `error` | `Cannot construct array with negative size -1.` |
| `ArraySizeExceeded` | `Runtime` | `error` | `Array size exceeds maximum.` |
| `FFIFunctionNotFound` | `Runtime` | `error` | `FFI function 'MIDORI_FFI_Foo' not found.` |
| `StackOverflow` | `Runtime` | `panic` | `Stack overflow - exceeded maximum call depth.` |
| `MemoryAccessViolation` | `Runtime` | `panic` | `Memory access violation.` |
| `DivisionByZero` | `Runtime` | `error` | `Division by zero.` |
| `InternalTypeError` | `Runtime` | `panic` | `Internal type error: expected Int, got Text.` |
| `InternalFFITypeError` | `Runtime` | `panic` | `Internal FFI type error.` |
| `UnsupportedPlatformOperation` | `Runtime` | `error` | `Operation not supported on this platform.` |
| `WorkerCancelled` | `Runtime` | `error` | `Worker cancelled.` |

## Stability

Version `1` is intended for editor and automation integration.

Stability guarantees:

- existing fields listed above are stable within schema version `1`
- new fields may be added in future versions without removing existing fields
- `file_path` and `caret_length` are compatibility fields and may be removed in
  a future schema version after a deprecation window
- example messages above are illustrative; the exact wording of any specific
  diagnostic may change between releases. Tooling should match on `code`, not
  on the human-readable `message`.
