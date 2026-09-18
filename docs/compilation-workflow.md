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
  -> OptimizerManager
  -> CodeGenerator
  -> BytecodeLinker
  -> VirtualMachine
```

The top-level driver preserves warnings and errors in a shared `CompilerReport`; diagnostics are rendered only at the CLI boundary. See [Error Reporting](error-reporting.md).

## Phase 1: Lexical Analysis

Source: `compiler/src/Compiler/Lexer/`

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

Source: `compiler/src/Compiler/ModuleManager/`

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

- System imports are resolved through `MARMOT_PATH`.
- Path imports are resolved relative to the importing file.
- Duplicate module names are rejected.
- Circular dependencies are rejected.
- If an imported file lives beside a `package.marmot`, the package manifest is loaded and any declared dynamic FFI library is registered before compilation continues.

The build graph stores:

- the stripped token stream for each module body
- source lines for diagnostics
- module-to-module dependencies
- collected `use` imports
- module declarations and export metadata

## Phase 3: Syntax Analysis

Source: `compiler/src/Compiler/Parser/`

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

Source: `compiler/src/Compiler/TypeChecker/`

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

Source: `compiler/src/Compiler/StaticAnalyzerManager/`

Static analysis runs after type checking and before optimization. It emits warnings without mutating the AST.

Current warning passes:

- `UnusedLocalDiagnostic`
- `ShadowingPolicyDiagnostic`
- `CaptureEscapeDiagnostic`

Warnings remain structured as `CompilerWarning` values and are appended to the compile-wide report.

## Phase 6: Optimization

Source: `compiler/src/Compiler/OptimizerManager/`

Optimization is AST-based and iterative.

Current pass order:

1. `ConstantFolding`
2. `StrengthReduction`
3. `ConstantBranchElimination`
4. `LocalConstantPropagation`
5. `DeadCodeElimination`
6. `CanonicalizationCleanup`
7. `ClosureLifting`
8. `TailCallOptimization`

The optimizer does not run just once. It reruns the pass list until either:

- no pass reports a change, or
- `OptimizerManager::s_max_iterations` is reached

The current fixpoint cap is `8`.

## Phase 7: Code Generation

Source: `compiler/src/Compiler/CodeGenerator/`

The code generator lowers the optimized typed AST into a per-module `BytecodeModule`.

Important opcode families in the current executable format:

- Constants: `LOAD_STRING`, `INTEGER_CONSTANT`, `FLOAT_CONSTANT`, `BYTE_CONSTANT`, `WORD_CONSTANT`, `OP_UNIT`, `OP_TRUE`, `OP_FALSE`
- Small integer constants: `INT_MINUS_1`, `INT_0`, `INT_1`, `INT_2`, `INT_3`, `INT_4`, `INT_5`, `INT_10`
- Arrays and tuples: `CREATE_ARRAY`, `CREATE_TUPLE`, `GET_ARRAY`, `GET_TUPLE`, `UNPACK_TUPLE`, `ADD_BACK_ARRAY`, `GET_ARRAY_LENGTH`
- Ranges: `CREATE_INT_RANGE`, `CREATE_FLOAT_RANGE`, `GET_RANGE_START`, `GET_RANGE_END`, `GET_RANGE_STEP`
- Casts: `INT_TO_FLOAT`, `TEXT_TO_FLOAT`, `FLOAT_TO_INT`, `TEXT_TO_INT`, `FLOAT_TO_TEXT`, `INT_TO_TEXT`, `WORD_TO_TEXT`, `BYTE_TO_INT`, `INT_TO_BYTE`, `BYTE_TO_WORD`, `WORD_TO_BYTE`, `WORD_TO_INT`, `INT_TO_WORD`, `BYTE_TO_FLOAT`, `FLOAT_TO_BYTE`, `WORD_TO_FLOAT`, `FLOAT_TO_WORD`
- Arithmetic and bit operations: `ADD_*`, `SUBTRACT_*`, `MULTIPLY_*`, `DIVIDE_*`, `MODULO_*`, `LEFT_SHIFT`, `RIGHT_SHIFT`, `BITWISE_AND`, `BITWISE_OR`, `BITWISE_XOR`, `BITWISE_NOT`
- Fused integer update: `ADD_ASSIGN_INT`, `SUB_ASSIGN_INT` (produced only when the code generator splits a fused `ADD_LOCAL_INT` back apart because that local became a captured cell; the language has no compound assignment)
- Control flow: `JUMP_IF_FALSE`, `JUMP_IF_TRUE`, `JUMP`, `JUMP_BACK`, fused compare-and-branch opcodes such as `IF_INTEGER_LESS` and `IF_FLOAT_GREATER_EQUAL`
- Pattern matching: `LOAD_TAG`, `GET_TAG`, `SET_TAG`
- Calls: `CALL_FOREIGN`, `CALL_FOREIGN_INDEXED`, `CALL`, `CALL_0` through `CALL_3`, `CALL_PROC`, `CALL_PROC_0` through `CALL_PROC_3`, `CALL_GLOBAL`, `CALL_GLOBAL_WIDE`, `TAIL_CALL`
- Data construction: `CONSTRUCT_STRUCT`, `CONSTRUCT_UNION`
- Closures and functions: `MAKE_FUNCTION`, `MAKE_CLOSURE`, `BIND_CAPTURES`
- Variables: `DEFINE_GLOBAL`, `GET_GLOBAL`, `SET_GLOBAL`, `GET_LOCAL`, `SET_LOCAL`, `GET_LOCAL_CELL`, `SET_LOCAL_CELL`, `GET_CELL`, `SET_CELL`, plus wide variants
- Cells (`Cell<T>`): `MAKE_CELL`, `READ_CELL`, `WRITE_CELL`, operating on a cell value on the stack rather than a captured local
- Members and stack: `GET_MEMBER`, `POP`, `DUP`, `SWAP`, `POP_LOCAL_SCOPE`, `POP_VALUES`, `POP_BLOCK_SCOPE`, `POP_MATCH_SCOPE`
- Termination: `RETURN`, `HALT`

Generic functions are specialized at call sites; the emitted module keeps specialization metadata so later codegen and linking stages can resolve the concrete procedures.

## Phase 8: Linking

Source: `compiler/src/Compiler/BytecodeLinker/`

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

Source: `compiler/src/Compiler/Compiler.cpp`

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
