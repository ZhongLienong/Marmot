# Marmot Compilation Workflow

This document describes the current compiler pipeline from `.mmt` source to linked bytecode.

## Overview

Marmot compiles in these stages:

```text
Source
  -> Lexer
  -> ModuleManager
  -> Parser
  -> TypeChecker
  -> StaticAnalyzerManager
  -> Lowering
  -> MidoriIROptimizer
  -> BytecodeBackend
  -> BytecodeLinker
  -> VirtualMachine
```

The top-level driver preserves warnings and errors in a shared `CompilerReport`; diagnostics are rendered only at the CLI boundary. See [Error Reporting](error-reporting.md).

## Phase 1: Lexical Analysis

Source: `projects/marmotc/src/Compiler/Lexer/`

The lexer converts raw source text into a `TokenStream`.

Current lexer behavior:

- Skips whitespace, `//` line comments, and `/* ... */` block comments.
- Block comments are not nested.
- Recognizes identifiers, keywords, literals, and symbolic operators.
- Tracks line, column, and token span for later diagnostics.
- Supports decimal integers, floats, `0x` hex integers, and `0b` binary integers.
- Supports string escapes such as `\n`, `\t`, `\\`, and `\"`.
- Rejects removed legacy shift spellings such as `<~` and `~>` with replacement hints.

Current token inventory includes:

- Keywords such as `def`, `fn`, `if`, `then`, `else`, `match`, `class`, `instance`, `type`, `alias`, `deriving`, `module`, `import`, `use`, `public`, `private`, and `foreign`.
- Type keywords such as `Int`, `Float`, `Byte`, `Word`, `Text`, `Bool`, `Unit`, `Array`, and `Never`.
- Operators such as `++`, `|>`, `::`, `as`, `==`, `!=`, `<=`, `>=`, `<<`, `>>`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, and `>>=`.

## Phase 2: Module Resolution

Source: `projects/marmotc/src/Compiler/ModuleManager/`

`ModuleManager` scans top-level module statements, resolves imports, and builds a dependency graph.

Key rules:

- Every `.mmt` file must contain exactly one explicit `module` declaration.
- The `module` declaration must be the first top-level statement in the file.
- `import`, `use`, `public export`, and `private export` can appear later and can be scattered across the file.
- Module statements are collected before normal parsing, so their relative placement after `module` does not change semantics.

Import forms:

- System import: `import { <IO> }`
- Path import: `import { "./helpers.mmt" }`

Resolution behavior:

- System imports are resolved through the search paths the compiler is given: a build plan's, or `MARMOT_PATH` when `marmotc` compiles a file on its own.
- Path imports are resolved relative to the importing file.
- Duplicate module names are rejected.
- Circular dependencies are rejected.
- A file belongs to a native package when the build plan names one whose root is the file's directory; it may then declare that package's foreign functions. The library itself is loaded only when the program runs.

The build graph stores:

- the stripped token stream for each module body
- source lines for diagnostics
- module-to-module dependencies
- collected `use` imports
- module declarations and export metadata

## Phase 3: Syntax Analysis

Source: `projects/marmotc/src/Compiler/Parser/`

The parser converts each module's `TokenStream` into a `MidoriProgramTree`.

The parser is a recursive-descent parser with precedence handling, contextual rewrites, and error recovery.

Current statement variants:

- `ExpressionStatement`
- `VariableDefinition`
- `TupleDefinition`
- `FunctionDefinition`
- `ForeignDefinition`
- `Struct`
- `Union`
- `Class`
- `Instance`
- `TypeAlias`

Current pattern variants:

- `Binding`
- `Wildcard`
- `Literal`
- `Tuple`
- `Array`
- `Constructor`

Current expression variants:

- `As`
- `Binary`
- `Group`
- `Tuple`
- `Literal` (one node; `m_kind` is `Bool`, `Integer`, `Byte`, `Word`, `Float`, `Text` or `Unit`)
- `UnaryPrefix`, `UnarySuffix`
- `Spawn`, `Join`, `ChannelCreate`, `Send`, `Receive`
- `NameAccess`
- `Call`, `Function`
- `Construct`, `RecordUpdate`
- `IfElse`
- `MemberAccess`
- `Array`, `IndexAccess`
- `ArrayComprehension`
- `RangeBinary`, `RangeTernary`
- `Block`
- `Match`, `Case`
- `For`

Notably absent:

- `async`
- `await`

Current parser features include:

- expression-oriented control flow
- generic parameter parsing
- `where` constraints on functions and type definitions
- associated type declarations and bindings in classes and instances
- `deriving (...)`
- pipe rewriting for `|>`
- pipe-into-`match`
- wildcard `_` pattern handling
- bidirectional constructor and lambda syntax that preserves omitted annotations for later inference

## Phase 4: Type Checking

Source: `projects/marmotc/src/Compiler/TypeChecker/`

See [Type System](type-system.md) for the language-level surface.

The type checker performs:

- Hindley-Milner style inference with bidirectional expected-type context
- registration of structs, unions, aliases, classes, instances, and associated types
- constraint solving and unification
- typeclass resolution
- exhaustiveness checking for `match`

It also records whether some operators should lower through typeclass dispatch, including:

- `as` through `Convertable<From, To>`
- `++` through `Concatenable<T>`
- `#` through `Countable<T>`
- `==` and `!=` through `Equatable<T>`
- `<`, `<=`, `>`, and `>=` through `Orderable<T>`

## Phase 5: Static Analysis

Source: `projects/marmotc/src/Compiler/StaticAnalyzerManager/`

Static analysis runs after type checking and before lowering. It emits warnings without mutating the AST.

Current warning passes:

- `UnusedLocalDiagnostic`
- `ShadowingPolicyDiagnostic`
- `CellCrossesWorkerDiagnostic`
- `IntegerOverflowDiagnostic`

Warnings remain structured as `CompilerWarning` values and are appended to the compile-wide report.

`marmotc check` and `marmotc build` accept `--emit-ast`, which is hidden from `marmotc --help`. It prints each module's checked AST, as it stands after static analysis, with how every name resolved (a local and its slot, a cell, or a global), in link order. It cannot be combined with `--format json`. With `--emit-ir` as well, every module's AST comes first, then every module's MidoriIR.

## Phase 6: Lowering

Source: `projects/marmotc/src/Compiler/Lowering/`

Lowering turns the checked AST of one module into MidoriIR, a typed SSA IR with one graph per function (see [MidoriIR](midori-ir.md)). Every top-level definition gets its global slot before any function is lowered. Generic functions are specialized here, on demand, one function per set of argument types, including generics imported from other modules; class methods and operators that dispatch through type classes are resolved to instance methods here too. Its diagnostics are reported under the `Lowering` stage.

The MidoriIR verifier checks the result. Development and Debug builds verify after lowering and after every optimizer pass, Release builds once, before the backend; a violation is a compiler bug, reported as `CompilerInternalError`.

## Phase 7: MidoriIR Optimization

Source: `projects/marmotc/src/Compiler/MidoriIROptimizer/`

The optimizer runs a fixed list of passes once over each module:

1. `DeadCodeElimination`
2. `SelfTailCall`
3. `ClosureConversion`
4. `Contification`
5. `Inlining`
6. `SelfTailCall`
7. `KnownConstructorThreading`
8. `DeadCodeElimination`
9. `ScalarReplacement`
10. `ParameterUnboxing`
11. `Sccp`
12. `StrengthReduction`
13. `GlobalValueNumbering`
14. `LoopInvariantCodeMotion`
15. `DeadCodeElimination`

It reports nothing. [MidoriIR](midori-ir.md#optimizer) describes each pass and what they all keep: tail calls, stack traces and source lines.

## Phase 8: Bytecode Emission

Source: `projects/marmotc/src/Compiler/BytecodeBackend/`

The bytecode backend turns each module's MidoriIR into a `BytecodeModule`. It chooses where each value lives (a frame slot shared by liveness, the operand stack, or nowhere) and which opcodes to use, superinstructions included. Its only diagnostics are the encoding's limits, reported as `CodeGeneratorLimitExceeded` under the `CodeGenerator` stage.

Important opcode families in the current executable format:

- Constants: `LOAD_STRING_WIDE`, `INTEGER_CONSTANT`, `FLOAT_CONSTANT`, `BYTE_CONSTANT`, `WORD_CONSTANT`, `OP_UNIT`, `OP_TRUE`, `OP_FALSE`
- Small integer constants: `INT_MINUS_1`, `INT_0`, `INT_1`, `INT_2`, `INT_3`, `INT_4`, `INT_5`, `INT_10`
- Arrays and tuples: `CREATE_ARRAY`, `CREATE_TUPLE`, `GET_ARRAY`, `GET_TUPLE`, `ADD_BACK_ARRAY`, `GET_ARRAY_LENGTH`
- Ranges: `CREATE_INT_RANGE`, `CREATE_FLOAT_RANGE`, `GET_RANGE_START`, `GET_RANGE_END`, `GET_RANGE_STEP`
- Casts: `INT_TO_FLOAT`, `TEXT_TO_FLOAT`, `FLOAT_TO_INT`, `TEXT_TO_INT`, `FLOAT_TO_TEXT`, `INT_TO_TEXT`, `WORD_TO_TEXT`, `BYTE_TO_INT`, `INT_TO_BYTE`, `BYTE_TO_WORD`, `WORD_TO_BYTE`, `WORD_TO_INT`, `INT_TO_WORD`, `BYTE_TO_FLOAT`, `FLOAT_TO_BYTE`, `WORD_TO_FLOAT`, `FLOAT_TO_WORD`
- Arithmetic and bit operations: `ADD_*`, `SUBTRACT_*`, `MULTIPLY_*`, `DIVIDE_*`, `MODULO_*`, `LEFT_SHIFT`, `RIGHT_SHIFT`, `BITWISE_AND`, `BITWISE_OR`, `BITWISE_XOR`, `BITWISE_NOT`
- Control flow: `JUMP_IF_FALSE`, `JUMP`, `JUMP_BACK`, fused compare-and-branch opcodes such as `IF_INTEGER_LESS` and `IF_FLOAT_GREATER_EQUAL`
- Local-operand superinstructions: `ADD_LOCAL_INT`, `STEP_LOCAL`, `PUSH_LOCAL_SUB_INT`, `IF_LOCAL_LE_INT`, `IF_LOCAL_LT_INT`, `IF_LOCAL_GE_LOCAL`, `IF_LOCAL_LT_LOCAL`, `IF_LOCAL_EQ_LOCAL`, `IF_LOCAL_TAG_NOT`, `LOCAL_UNION_FIELD`, `LOCAL_ARRAY_GET`, `LOCAL_UNION2`, `APPEND_LOCAL`, `GET_LOCAL2`, `STORE_LOCAL`
- Pattern matching: `GET_TAG`, `GET_UNION_FIELD`
- Calls: `CALL_FOREIGN`, `CALL_FOREIGN_INDEXED`, `CALL`, `CALL_0` through `CALL_3`, `CALL_PROC_WIDE`, `CALL_GLOBAL_WIDE`, `TAIL_CALL`
- Data construction: `CONSTRUCT_STRUCT`, `CONSTRUCT_UNION`
- Closures and functions: `MAKE_FUNCTION_WIDE`, `MAKE_CLOSURE_OF`, `SET_CAPTURE`, `GET_CELL`, `GET_CELL_WIDE`
- Variables: `DEFINE_GLOBAL_WIDE`, `GET_GLOBAL_WIDE`, `SET_GLOBAL_WIDE`, `GET_LOCAL`, `SET_LOCAL`, `GET_LOCAL_WIDE`, `SET_LOCAL_WIDE`
- Cells (`Cell<T>`): `MAKE_CELL`, `READ_CELL`, `WRITE_CELL`, operating on a cell value on the stack rather than a captured local
- Members and stack: `GET_MEMBER`, `POP`, `PUSH_PLACEHOLDER`
- Termination: `RETURN`, `HALT`

Procedures, globals and text constants are named only in two bytes, by the `WIDE` forms, because the linker adds each module's first procedure, global and text to them. A closure is `MAKE_CLOSURE_OF`, which takes exactly the values it captures. `OpCodes.def` is the list of every opcode and its length; removing, reordering or resizing one changes `MbcFormatVersion`, and the VM refuses a `.mmc` of another version.

## Phase 9: Linking

Source: `projects/marmotc/src/Compiler/BytecodeLinker/`

`BytecodeLinker` merges per-module bytecode into a single `MidoriExecutable`.

Linking performs:

- procedure/global/string-pool offset assignment
- export collection
- duplicate export checks
- import patching
- procedure concatenation
- bootstrap generation

The linker works on modules in build-schedule order, which is deterministic even when compilation ran in parallel.

## Scheduling Model

Source: `projects/marmotc/src/Compiler/Compiler.cpp`

The compiler derives stable compilation tiers from the dependency graph, but the actual scheduler is dependency-driven rather than tier-blocked.

Current behavior:

- `BuildGraph::GetCompilationTiers()` is used for deterministic progress reporting and final linking order.
- The compiler builds a ready queue from modules whose dependencies are already satisfied.
- On native builds, workers are `std::jthread` instances that pull from the queue.
- When a module completes, any newly unblocked dependents are enqueued immediately.
- On Emscripten builds, the same dependency logic runs through a single-threaded queue.

## FFI Notes

The runtime has two FFI call paths:

- `CALL_FOREIGN_INDEXED` for built-in runtime FFI entries declared in `MidoriFFIRegistry`
- `CALL_FOREIGN` for dynamically loaded functions, including package-provided libraries

The indexed path carries explicit argument and return metadata such as `CString`, `ArrayView`, `TraceableHandle`, `ValueHandle`, `ArrayValues`, and `ArrayStrings`. The dynamic path is more generic and is documented in [Package System](package-system.md).

## Diagnostics and Reporting

Every phase reports through structured diagnostics instead of printing directly.

Current top-level behavior:

- successful compilations can still return warnings
- warnings from earlier stages survive later failures
- the driver renders warnings before errors
- machine-readable warnings and JSON reports are derived from the same underlying `CompilerReport`

See [Error Reporting](error-reporting.md) for the exact shapes.
