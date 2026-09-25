# Marmot Feature Matrix

This document is the tracked source of truth for the current public Marmot surface.
Status is based on the Phase 1 audit and the repo state checked on 2026-04-04.

The coverage column points at the primary automated coverage for each row. It is
not an exhaustive list of every relevant fixture.

See [Versioning Policy](versioning-policy.md) for how `Stable`,
`Experimental`, and `Planned` affect compatibility and releases.

## Stability Levels

- **Stable**: implemented on its relevant end-to-end path, documented in tracked docs, and covered by automated tests. For language syntax, that normally means the lexer/parser/typechecker/codegen/runtime path exists and at least one `test/` or `tests/` fixture exercises it.
- **Experimental**: implemented, but still limited, early, or thinly covered compared with the rest of the language.
- **Planned**: not implemented in the current token, AST, parser, typechecker, code generator, or runtime surface.

## Type System

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Primitive values: `Int`, `Float`, `Byte`, `Word`, `Bool`, `Text`, `Unit` | Stable | `test/literal/`, `test/ffi/`, `runtime/tests/runtime/` | `Text` is UTF-8 and remains the only built-in string type. |
| Bottom type: `Never` | Stable | `test/prelude/`, `test/hashmap/` | Mostly exercised through `Prelude/Panic` and bottom-type unification rather than a dedicated `Never` fixture. |
| Arrays and tuples | Stable | `test/literal/`, `test/expression/`, `compiler/tests/parser/`, `runtime/tests/runtime/` | Tuple literals and tuple destructuring both have dedicated regression coverage, with parser/runtime unit tests still covering AST and VM details. |
| Structs and unions | Stable | `test/struct/`, `test/union/`, `test/match/` | Product and sum types both compile through the normal end-to-end path. |
| Type aliases | Stable | `test/type_alias/`, `test/generics/` | `alias X = Y` is transparent. The older transparent `type X = Y` has been migrated; `type` is now nominal. Parameterised aliases are limited — see the type-declaration row. |
| `type` declarations | Stable | `test/type_declaration/`, `test/newtype/` | One keyword for three shapes: `type P = { x: Int }` record, `type O<T> = A \| B(T)` sum, `type Meters = Int` newtype. The right-hand side selects the kind. `struct` and `union` have been removed; a file still using either gets a parser diagnostic naming the replacement. |
| Newtypes | Stable | `test/newtype/` | `type Meters = Int` is nominal — an `Int` does not pass where `Meters` is expected. Erased at opcode selection, so zero runtime cost. Typeclass instances attach to the newtype independently of its representation. Generic newtypes are declaration-only. |
| First-class functions and lambdas | Stable | `test/generics/`, `test/pipe/`, `test/closure/` | Includes anonymous functions and contextual lambda inference. |

## Expressions and Control Flow

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Pattern guards | Stable | `test/match/`, `compiler/tests/` | `case P if cond => e`. A failed guard falls through to the next arm, including one with the same constructor. A guarded arm does **not** count toward exhaustiveness, since the guard is a runtime test. |
| Record update | Stable | `test/struct/` | `{ s with f = v, g = w }` copies a record with fields replaced. Simultaneous — right-hand sides see the original. Duplicate fields are an error, nested paths are not supported. Works inside generic functions, taking its type from the already-resolved source record. |
| `if`, block expressions, `match`, `for` | Stable | `test/expression/`, `test/for_loop/`, `runtime/tests/runtime/` | Every control-flow form is an expression. `return`, `loop`, `break` and `continue` were removed; using one is a compile error naming the replacement. |
| `for ... in` over ranges, arrays, and `Iterable` implementations | Stable | `test/for_loop/`, `test/prelude/` | `Iterable`-backed loops use `Iterable::Next` at type-check and codegen time. |
| Binary and ternary ranges | Stable | `test/range/`, `test/for_loop/`, `runtime/tests/runtime/` | Both `start..end` and `start..step..end` are implemented. |
| Array comprehensions | Stable | `test/array_comprehension/`, `compiler/tests/parser/` | Supports range, array, and `Iterable` inputs. |
| Pipe operator: `|>` and `|> match with` | Stable | `test/pipe/` | Pipe rewriting is handled in the parser. |
| Closures and captured mutation | Stable | `test/closure/`, `runtime/tests/runtime/`, `compiler/tests/static_analyzer/` | Captured locals are boxed so nested closures preserve by-reference semantics. |

## Pattern Matching

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| `match` with binding, wildcard, literal, tuple, array, and constructor patterns | Stable | `test/match/`, `compiler/tests/parser/`, `compiler/tests/typechecker/` | The current pattern inventory is six variants. |
| Exhaustiveness checking | Stable | `test/match/`, `compiler/tests/typechecker/` | Exhaustiveness is currently top-level only. |

## Generics and Typeclasses

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Generic functions, structs, unions, and aliases | Stable | `test/generics/`, `test/type_alias/` | Includes nested generics and multi-parameter definitions. |
| `where` constraints and type-definition constraints | Stable | `test/generics/`, `test/typeclass/` | Constraints work on functions and type definitions. |
| Constructor inference and context-sensitive lambda typing | Stable | `test/generics/`, `test/pipe/` | The type checker uses bidirectional context here. |
| Classes, instances, and associated types | Stable | `test/typeclass/`, `compiler/tests/typechecker/` | Includes associated type declarations and instance bindings. |
| Cross-module typeclass metadata and imports | Stable | `test/typeclass/`, `test/module/` | Typeclass/import metadata survives module boundaries. |
| Deriving: `Equatable`, `Hashable`, `Map`, `Bind`, `Unwrap` | Stable | `test/deriving/` | Structural deriving is limited to focused non-generic, non-recursive shapes; container deriving supports focused union shapes. |
| Operator-backed typeclass dispatch: `Convertable`, `Concatenable`, `Countable`, `Equatable`, `Orderable` | Stable | `test/typeclass/`, `test/prelude/`, `test/as_operator/` | The implementation mixes builtin lowering with constrained dispatch depending on the concrete types. |

## Operators

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Arithmetic, logical, bitwise, and comparison operators | Stable | `test/expression/`, `test/typeclass/`, `compiler/tests/lexer/` | User-defined equality and ordering rely on `Equatable` and `Orderable`. |
| Cast operator: `as` | Stable | `test/as_operator/`, `test/typeclass/` | Covers builtin primitive casts and constrained generic conversions. |
| Concatenation: `++` for `Text` and `Array<T>` | Stable | `test/expression/`, `test/prelude/`, `test/typeclass/` | Constrained generic code can lower through `Concatenable<T>`. |
| Length operator: `#` | Stable | `test/for_loop/`, `test/prelude/`, `test/hashmap/`, `test/hashset/` | Arrays are builtin; `List`, `Map`, `Set`, and generic `Countable` paths also exist. |
| Compound assignment: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` | Stable | `test/expression/`, `compiler/tests/lexer/` | End-to-end regression coverage now exercises numeric and bitwise compound assignment on locals and struct members, plus type-check failures for unsupported targets. |

## Modules, FFI, Packages, and Diagnostics

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Modules, path imports, system imports, `use`, exports, privacy, and qualified access | Stable | `test/module/`, `compiler/tests/module/`, `compiler/tests/parser/` | `module` must be first; import/use/export blocks can appear later and be scattered. |
| Module diagnostics: circular imports, unresolved imports, missing exports, duplicate modules | Stable | `test/module/`, `compiler/tests/module/`, `compiler/tests/compiler/` | The compiler emits stable module error codes for these cases. |
| `foreign` declarations and builtin runtime FFI (`CALL_FOREIGN_INDEXED`) | Stable | `test/ffi/`, `test/prelude/` | This is the richer built-in FFI path backed by `MidoriFFIRegistry`. |
| Dynamic package FFI (`CALL_FOREIGN`) | Experimental | `none yet` | The generic ABI exists, but package-specific automated coverage is still thin and the dynamic path does not expose the full builtin typed-FFI metadata. |
| Native libraries (`foreign ... from "library"`, `package.marmot` `[ffi]`) | Experimental | `compiler/tests/utility/NativeLibraryTests.cpp`, `tool/src/tests.rs`, `scripts/testing/cli_contracts.py` | Source names the library; the `.mmc` records it with its `thread_safe` flag and checksum. `marmotvm` finds and loads it before the program starts through `--library`, `--library-path`, `MARMOT_LIBRARY_PATH` or the declaring module's directory; `marmotc` never does. |
| Structured compiler warnings/errors and stable diagnostic codes | Stable | `common/tests/common/`, `compiler/tests/compiler/`, `test/static_analyzer/` | Warnings and errors are aggregated in `CompilerReport` instead of being printed ad hoc. |
| Machine-readable warnings and compiler-report JSON | Stable | `common/tests/common/`, `test/static_analyzer/`, `scripts/testing/cli_contracts.py` | `*.warnings.json` fixtures exercise the warning-stream path, and CLI contract checks cover `marmotc.exe check --format json`. |
| Static-analyzer warnings: `UnusedLocal`, `ShadowingPolicy`, `CaptureEscape`, `IntegerOverflow` | Stable | `compiler/tests/static_analyzer/`, `test/static_analyzer/` | Warnings are preserved even when a later compile stage fails. |
| Projects (`project.marmot`, `[project]` fallback, `marmot.lock`), package resolution and `marmot init` | Experimental | `tool/tests/` | The `marmot` tool's tests cover manifest lookup and precedence, resolution, vendoring, lockfiles, test discovery and scaffolding. |

## Standard Library

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| Prelude ADTs: `Option`, `Result`, `List` | Stable | `test/prelude/`, `test/match/` | These are the core public prelude data types. |
| Typed `IO`, `System`, and `DateTime` wrappers | Stable | `test/prelude/` | The public surface prefers `Option` and `Result` wrappers over sentinel values. |
| `TextUtil`, `ArrayUtil`, and `Math` helpers | Stable | `test/prelude/`, `test/ffi/` | These sit above the lower-level builtin FFI helpers. |
| `Collections/Map` and `Collections/Set` | Stable | `test/hashmap/`, `test/hashset/` | Persistent: `With` and `Without` return a new collection. Keys rely on `Hashable` and `Equatable`. |
| Helper/typeclass modules: `Concatenable`, `Convertable`, `Countable`, `Equatable`, `Hashable`, `Iterable`, `Orderable`, `Prelude/Panic` | Stable | `test/prelude/`, `test/typeclass/`, `test/hashmap/`, `test/hashset/` | Some modules mostly define reusable class surfaces and expect user code to supply instances. |

## Concurrency

| Feature | Status | Primary Coverage | Notes |
|---------|--------|------------------|-------|
| `Concurrency::Spawn`, `Concurrency::Join` | Stable | `test/concurrency/` | Compiler-provided functions, available once `Concurrency.mmt` is imported. `Spawn(argument, F)` takes `F` as a value - a name, a lambda, or a parameter holding one - and spreads a tuple argument across several parameters, so `(a, b) \|> Concurrency::Spawn(F)` pipes. `Concurrency::ParallelMap` is built on it, in Marmot. `Concurrency::Close`, `IsDone` and `Cancel` round out the surface. `Join` evaluates to `Result<T, WorkerError>`, so a failed or cancelled worker is a value rather than an error in the joiner (needs `Prelude/Result.mmt` too). Callee must be a named `def Name = fn(...)`. The `spawn`/`join` keywords were removed. |
| `Concurrency::MakeChannel(cap)` | Stable | `test/concurrency/` | Creates a typed bounded channel, taking `T` from context (`def ch : Channel<Int> = ...`). `T` must satisfy `Transferable`. Replaces the `channel<T>(cap)` keyword. |
| `->` (send) and `<-` (receive) operators | Stable | `test/concurrency/` | Binary send and unary prefix receive; type-checked against `Channel<T>`. |
| `Worker<T>` and `Channel<T>` types | Stable | `test/concurrency/` | Opaque handle types with compile-time type parameter tracking. |
| Constrained instances | Stable | `test/typeclass/` | `instance C<T> where D<T>` — an instance may require constraints on its own type parameters, resolved recursively and across module boundaries. |
| `Indexable<C, I>` typeclass | Stable | `test/typeclass/` | `x[i]` resolves through `Indexable` for any type with an instance, arrays included. Two type parameters, so a container may be indexed by something other than `Int`. |
| Generic lambdas | Stable | `test/generics/` | `fn<T>(x: T) -> T where C<T> => e`, bound to a top-level `def`. Anonymous, capturing and nested generic lambdas are rejected with a diagnostic — only a module-level binding is supported. |
| `Transferable<T>` typeclass | Stable | `test/concurrency/` | Built-in instances for primitives, `Array<T>`, and `Channel<T>`. Derivable for structs and unions. |
| `deriving (Transferable)` | Stable | `test/concurrency/` | Generates field-by-field serialization for structs and tag+payload serialization for unions. |
| Auxiliary operations: `close`, `is_done`, `cancel` | Stable | `test/concurrency/` | Parsed as normal function calls; emit dedicated opcodes. |
| Non-blocking / bounded receive (`try_receive`, `select`, timeouts) | Not implemented | — | `Channel::TryReceive` exists in the runtime but has no opcode or syntax; `try_receive(ch)` is an undefined name. |
| Worker cancellation (`cancel`) | Stable | `test/concurrency/`, `runtime/tests/runtime/WorkerCancellationTests.cpp` | Cooperative: observed at loop back-edges, tail calls, foreign-call returns, and blocking channel waits. Blocking stdin and third-party FFI are not interruptible. |
| Isolated-worker runtime | Stable | `test/concurrency/`, `runtime/tests/runtime/` | Per-VM isolation of heap, GC, stack, globals, and string cache. Single-threaded cost is one never-taken branch at cancellation safepoints. |
