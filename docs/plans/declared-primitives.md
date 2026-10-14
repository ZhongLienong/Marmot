# Declared primitives: no names the compiler recognises by spelling

Branch `declared-primitives`. Goal: nothing in the compiler finds a library
function or type by how its name is spelled. Today six concurrency operations are
VM instructions that the language has no way to declare, so the compiler
recognises the text `Concurrency::X` in three stages instead:

- the parser keeps a list of the six names, rewrites three of the calls into their
  own syntax-tree nodes (`Spawn`, `Join`, `ChannelCreate`), looks up `Result` and
  `WorkerError` by name for `Join`, and lets the six names past the import check
  (`IsModuleVisible`), a second resolution path beside the ordinary one;
- the type checker matches the six names in `CheckCall`, partly only to report the
  arity of a call the parser did not rewrite, and checks `Result` and
  `WorkerError` against the member names `Ok`, `Err`, `Cancelled` and `Failed`;
- lowering keeps two more lists of three names;
- the static analyzer warns about `Concurrency::ParallelMap` by name.

The user can see it: `def close = Concurrency::Close;` says "variable not found",
`use Concurrency.{ MakeChannel }` says the symbol is not exported, and calling
`Join` without importing `Result.mmt` is a parse error.

## Decisions (2026-10-13)

- **A primitive is declared.** `foreign "MIDORI_PRIMITIVE_<Op>" Name: type;` with
  no `from`, the form a builtin already uses. The symbol names a MidoriIR
  instruction instead of an entry in the VM's builtin table. Imports, exports,
  `use` and type checking treat the declaration like any other. No grammar change.
- **The compiler knows a primitive only by its declaration.** The parser records
  the primitive on the declared function type, from the declaration's own symbol.
  The type checker marks each call whose callee names a primitive declaration;
  lowering emits the instruction for a marked call. Nothing anywhere compares a
  name's spelling.
- **A primitive can only be called**, as a Go builtin can only appear in a call.
  None of the six can be a value today, and `Spawn`'s typing is not its signature,
  so as a value it would skip the transferability checks.
- **Two primitives keep a typing rule their signature cannot state**, chosen by the
  primitive and not by name. `Spawn(argument, F)` spreads one argument across F's
  zero, one or several parameters, and requires both sides transferable.
  `MakeChannel(capacity)` takes its element type from the expected type and
  requires it transferable. The rest are typed by their signature alone.
- **`Join`'s result has a positional contract**, checked once at its declaration:
  a union whose member 0 carries the worker's value and member 1 an error, a union
  whose member 0 has no fields and member 1 carries `Text`. The VM builds those
  tags without being told them, so `JOIN_WORKER` loses its four tag operands and
  MidoriIR loses `MidoriIRJoinTags`: format 16.
- **The cell-crossing warning follows summaries, not names.** Each function's
  summary says which parameters reach a worker and whether its result comes back
  from one; modules export them; a call to any summarised function warns as a call
  to `ParallelMap` does today.
- **Workflow:** this branch, one commit per phase, merged once the gate passes on
  Dev and Debug, as for the precise-values work.

## Phase 1: a call result whose type is never decided is an error

`def b = Make(3);` with `Make : fn<T>(Int) -> Array<T>` passes the type checker and
stops the backend: "cannot tell whether a T1 is a pointer". The same happens for a
local definition and an expression statement. A later use that decides `T` works
today and must keep working, so the check runs once the module is checked: a call
whose resolved type still holds a type variable that is not a generic parameter
of some definition is an error at the call. Generic parameters are rigid only
while their definition is checked, so the checker keeps the ids of every generic
parameter it has made rigid.

Exit: a fixture under `test/generics/failure/` with the three forms; gate green.

## Phase 2: the six concurrency operations are declared primitives

`MarmotPrelude/Concurrency.mmt` declares and exports:

```
foreign "MIDORI_PRIMITIVE_Spawn" Spawn: fn(A, fn(A) -> R) -> Worker<R>;
foreign "MIDORI_PRIMITIVE_Join" Join: fn(Worker<T>) -> Result<T, WorkerError>;
foreign "MIDORI_PRIMITIVE_ChannelNew" MakeChannel: fn(Int) -> Channel<T>;
foreign "MIDORI_PRIMITIVE_ChannelClose" Close: fn(Channel<T>) -> Unit;
foreign "MIDORI_PRIMITIVE_WorkerIsDone" IsDone: fn(Worker<T>) -> Bool;
foreign "MIDORI_PRIMITIVE_WorkerCancel" Cancel: fn(Worker<T>) -> Bool;
```

- `Compiler/Primitive/` holds the enum and the one table from symbol to primitive.
- Parser: a builtin-form `foreign` whose symbol is a primitive marks its function
  type. The name list, `LowerConcurrencyIntrinsic`, the pipe's special case for the
  three nodes, the `Result`/`WorkerError` lookup and `IsModuleVisible`'s use for
  intrinsics go.
- Type checker: a primitive declaration's type is checked against what its
  instruction needs, so a wrong declaration is an error and not a VM fault. A
  primitive named as a value is an error. `Call::m_primitive` is set for a call to
  one; `Spawn` and `MakeChannel` take their rules. The name checks in `CheckCall`
  and the three node checkers go.
- Lowering: one switch from primitive to instruction. A primitive declaration
  reserves no global and exports no symbol. The name lists go.
- Syntax tree: `Spawn`, `Join` and `ChannelCreate` go, with their printer, walker
  and resolver cases.
- `JOIN_WORKER` is `[op][value kind]`; the VM builds `Ok` as tag 0 and `Err` as tag
  1, `Cancelled` as 0 and `Failed` as 1. Format 16: registry and contract renamed,
  fixtures regenerated.
- Errors keep their text and position where the construct still exists: they name
  the callee as it was written and point at it. `join_requires_result_import`
  stops being a failure, since `Join` no longer needs the file to name `Result`.

Exit: gate green on Dev and Debug. `def close = Concurrency::Close;` is a clear
error; `use Concurrency.{ MakeChannel }` works; a file joins without importing
`Result.mmt`; a declaration of a primitive with the wrong type is an error.

## Phase 3: worker-crossing summaries replace the `ParallelMap` name

A summary per top-level function: the parameters whose values reach a worker,
and whether its result comes back from one. A parameter reaches a worker when an
expression that reads it is an argument of `Spawn`, a sent value, or an argument
in a crossing position of a summarised call, followed through local definitions;
reads inside a lambda count, since the lambda carries them. A result comes back
from a worker when it is built from a `Join`'s result. Within a module the
summaries are found by iterating to a fixpoint; imported ones come through
`ModuleInterface`.

A parameter whose declared type already holds a cell is left out of the summary:
the warning was given inside the function, where the cell crossed. A function-typed
parameter is always kept, since what a function captured is not in its type.

At a call to a summarised function, or to `Spawn` or `Join`: a crossing argument
that holds a cell warns, a crossing function value warns about the cells it
carries, and a result from a worker that holds a cell warns. `Spawn` and `Join`
are summarised by their primitive. The per-node handler and `ParallelMapName` go.

Exit: the analyzer's snapshots unchanged except where a warning is new and right;
a fixture for a user function that spawns its parameter; gate green.

## Phase 4: finish

Docs that described compiler-provided functions, then this plan, go. Merge.

## Found along the way, not fixed here

- A `where` clause on a call is checked before the call's expected type can decide
  its types, so `def ch : Channel<Int> = MC(4)` fails for
  `MC : fn<T>(Int) -> Channel<T> where Transferable<T>`. Once it is not,
  `MakeChannel` can be ordinary Marmot and lose its typing rule.
- An instance constraint whose types are never decided stays pending and is never
  reported (`m_pending_instance_constraints`).
- Library names other features bind to by bare name: `Transferable`, `Convertable`,
  the `derive` targets and the associated type `Item`. Binding a language feature
  to a library declaration is ordinary; finding it by spelling is the same smell.

## Status

Phase 0 (this plan): done.

Phase 1: done. The check runs as `ResolveRecordedTypes` reaches each call, and
only the call that brings a variable in reports it: one whose argument already
holds the variable, as `Id(Make(3))` does, is not where the type went missing.
The message does not show the variable, whose number would pin every unrelated
change to the checker in the snapshot. Fixtures:
`generics/failure/call_result_never_decided` and
`generics/success/call_result_decided_by_later_use`.
