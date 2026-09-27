# MidoriIR

MidoriIR is the compiler's typed SSA intermediate representation. Lowering builds it from the checked AST after generics are specialized, the MidoriIR optimizer rewrites it, and the bytecode backend emits it. It replaces the AST optimizer and the code generator. Until it is complete, the AST path stays the default and MidoriIR is reached only with `marmotc --backend ir` (see [Command line](#command-line)).

Source: `compiler/src/Compiler/MidoriIR/`.

Change this document in the same change as the IR.

## Structure

| Type | What it holds |
| --- | --- |
| `MidoriIRModule` | One module: its global table, its functions, and which function runs the top-level statements (`m_top_level`) |
| `MidoriIRFunction` | A name, a return type, the types of its captures, its blocks, and a value table indexed by `MidoriIRValueId` |
| `MidoriIRBlock` | Block parameters and a list of instructions, the last of which is its terminator |
| `MidoriIRInstruction` | `m_op`, `m_result`, `m_type`, `m_operands`, `m_immediate`, `m_successors`, `m_effect`, `m_line` |
| `MidoriIRValue` | A value's type and an optional name, used only for printing |

Every generic specialization, instance method and lambda is its own function. Every value has a concrete type, because the IR is built after monomorphization. Types are the checker's `MidoriType`, compared as they are at run time: a newtype is its representation, so converting between them is no instruction, and `AddInt` takes a `Meters`.

- **Parameters.** The entry block, `bb0`, is the function's first block. Its parameters are the function's parameters.
- **Captures.** A function reads its captures with `GetCapture #i`. `m_capture_types` gives their types.
- **Source module.** A specialization of another module's generic runs that module's source; its `m_source_module` names the module, so a runtime error in it points at that module's file.
- **Block parameters instead of phi nodes.** A value that differs by the path taken is a parameter of the block where paths meet, and each jump passes it as an argument.
- **Every instruction but a terminator defines exactly one value.** An instruction run only for its effect, such as `GlobalSet`, defines a `Unit`.
- **Definitions are found from the blocks.** The value table does not record where a value is defined, so a pass that moves an instruction has nothing else to update.
- **Globals.** A global's slot is its index in the module's global table. Lowering reserves the slot of every top-level definition before it lowers any function. A global whose `m_module` names another module is imported from there: this module may read and call it, but not define or set it.
- **Source lines.** Every instruction keeps the line it came from, including after it is inlined into another function. The backend uses it for runtime error locations.

Functions, blocks and globals are kept in vectors, in the order lowering creates them, so output never depends on hash order.

## Instructions

`compiler/src/Compiler/MidoriIR/MidoriIROps.def` lists every instruction with its signature and default effect. The operation enum, the printed names, the fixed signatures the verifier checks and the builder's effects are all generated from that file.

| Family | Instructions | Operands and immediate |
| --- | --- | --- |
| Constants | `Const` | An immediate of the result's type: Int, Float, Byte, Word, Bool, Text, or none for Unit |
| Int arithmetic | `AddInt` `SubInt` `MulInt` `DivInt` `ModInt` `NegInt` | Int wraps |
| Float arithmetic | `AddFloat` `SubFloat` `MulFloat` `DivFloat` `ModFloat` `NegFloat` | |
| Byte and Word arithmetic | `AddByte` ... `ModByte`, `AddWord` ... `ModWord` | |
| Bit operations | `BitAnd` `BitOr` `BitXor` `BitNot` `Shl` `Shr`, each for `Int`, `Byte` and `Word` | A shift's amount is always an Int |
| Comparisons | `Eq` `Ne` `Lt` `Le` `Gt` `Ge` for `Int`, `Float`, `Byte` and `Word`; `EqBool` `NeBool` `NotBool`; `EqText` `NeText` | Give Bool |
| Conversions | `IntToFloat` `FloatToInt` `IntToText` `FloatToText` `WordToText` `TextToInt` `TextToFloat` `ByteToInt` `IntToByte` `ByteToWord` `WordToByte` `WordToInt` `IntToWord` `ByteToFloat` `FloatToByte` `WordToFloat` `FloatToWord` | |
| Tuples | `MakeTuple`, `TupleGet #i` | |
| Arrays | `MakeArray`, `ArrayGet array, index`, `ArrayLength` | |
| Structs | `Construct`, `GetMember #i`, `RecordUpdate #i record, value` | Members by position |
| Ranges | `MakeRange start, step, end`, `RangeStart` `RangeEnd` `RangeStep` | |
| Unions | `MakeUnion tag t`, `GetTag`, `UnionField tag t #i` | |
| Text and array growth | `Concat`, `Extend`, `ArrayAppend array, value` | `Concat` and `Extend` take and give one type, Text or an array. `Extend` and `ArrayAppend` change their left operand in place, so only where it is provably fresh, and give it back |
| Calls | `Call f`, `CallGlobal @slot`, `CallForeign foreign "name"`, `CallValue closure, ...` | Arguments follow. A `CallForeign` with no immediate calls the foreign function its first operand, a Text, names |
| Closures | `MakeClosure f` with the captures as operands, `BindCaptures #i closure, value`, `GetCapture #i` | `MakeClosure` may leave out captures at the end, which `BindCaptures` fills before the closure is called; that is how local functions that name themselves or each other reach one another |
| Cells | `CellNew`, `CellRead`, `CellWrite cell, value` | Only for the language's `Cell<T>`. `CellWrite` gives the value it writes |
| Globals | `GlobalDefine @slot`, `GlobalGet @slot`, `GlobalSet @slot` | |
| Concurrency | `Spawn args..., function`, `Join tags ok err cancelled failed`, `WorkerCancel` `WorkerIsDone` `ChannelNew` `Send` `Receive` `ChannelClose` | `Join` builds its Result from the union tags its immediate names |
| Terminators | `Jump`, `Branch`, `Switch`, `Return`, `TailCall`, `Unreachable` | See below |

A terminator ends its block and defines no value. Its successors carry their jump arguments.

| Terminator | Meaning |
| --- | --- |
| `jump bbN(args)` | Go to `bbN` |
| `branch cond, bbT(args), bbF(args)` | Go to `bbT` when the Bool `cond` is true, else to `bbF` |
| `switch union, t: bbN(args), ..., default: bbD(args)` | Go to the case for the union's tag; the default is optional |
| `return value` | Return from the function |
| `tailcall callee, args` | Call without a new frame: a function, a global `@slot`, or, with neither, a closure given as the first operand |
| `unreachable` | Nothing runs here: it follows a call that returns `Never`, and ends a match's last failed test, which the type checker's exhaustiveness makes impossible |

The language promises that every call in tail position runs without a new frame, so a call lowered in tail position stays a `TailCall` through every pass, including inlining.

## Effects

Every instruction has one effect. The builder takes it from `MidoriIROps.def`, and a pass may weaken it once it proves more.

| Effect | Printed | Meaning | DCE |
| --- | --- | --- | --- |
| pure | nothing | No effect | May drop |
| fault(kind) | `!fault(Index)` | May stop the program: `Index`, `DivisionByZero` or `Conversion` | Keeps it unless it proves the fault cannot happen, e.g. a non-zero constant divisor |
| alloc | `!alloc` | Allocates | May drop |
| read | `!read` | Reads what a write elsewhere may change (`GlobalGet`, `CellRead`) | May drop, but not move across an `io` or `call` |
| io | `!io` | Writes state, or talks to the outside | Always keeps |
| call | `!call` | Calls a function | Keeps it until the callee's own effect is known |

## Verifier

`MidoriIRVerifier(module).Verify()` returns every violation it finds, each with its rule number, function, block and a message. Development builds are to run it after lowering and after every pass, and Release builds once before the backend. A violation is a compiler bug, never a user error.

1. **Terminators.** Every block ends in exactly one terminator, no terminator appears anywhere else, and every function has a block. A call that returns `Never` is followed by `unreachable`.
2. **Dominance.** Every value is defined once, and every use is dominated by its definition. A use in the defining block must come after it.
3. **Successor arguments.** Every successor names an existing block. Its arguments match that block's parameters in number and type.
4. **Operand types.** Each instruction's operands, immediate and result have the types it expects: `AddInt` takes two Ints and gives an Int, a `Call` matches its callee's parameters and return type, a `Return` gives the function's return type, and so on. An instruction's type is its value's type, and only terminators have successors. Nothing uses a `Never` value. A tail call of a function that returns `Never` may end a function of any return type. A `MakeClosure`'s operands match the first of its function's captures. The concurrency instructions and `BindCaptures` are not checked yet.
5. **Switch coverage.** A `Switch` takes a union, has at most one case per tag and at most one default, and names only tags of that union. With no default, it covers every member.
6. **Global slots.** Every `GlobalDefine`, `GlobalGet`, `GlobalSet`, `CallGlobal` and global `TailCall` names a reserved slot, and no `GlobalDefine` or `GlobalSet` names an imported global.

Rules 2 to 6 hold in the blocks the entry reaches. A block nothing reaches holds code lowered after a call that returns `Never`, which never runs and which the backend does not emit, so only rule 1 applies to it. When lowering breaks a rule, the error prints each function it found a violation in.

## Textual form

`MidoriIRPrinter(module).Print()` writes a module like this:

```text
module Sum

fn Sum$Int(Array<Int>) -> Int
bb0(xs: Array<Int>):
  n: Int = ArrayLength xs
  zero: Int = Const 0
  jump bb1(zero, zero)
bb1(i: Int, acc: Int):
  c: Bool = LtInt i, n
  branch c, bb2, bb3
bb2:
  x: Int = ArrayGet xs, i  !fault(Index)
  a2: Int = AddInt acc, x
  one: Int = Const 1
  i2: Int = AddInt i, one
  jump bb1(i2, a2)
bb3:
  return acc
```

- **The module header.** The module's name comes first. Then come its globals by slot, as `global @0 total: Int`, with an imported global's module before its name, as `global @1 Lib::Scale: fn(Int) -> Int`, and, if it has one, the function that runs its top-level statements, as `top-level Sum$top`.
- **Function headers.** Each function starts with `fn`, its name, its parameter types and its return type, followed by `captures(...)` when it has captures.
- **Blocks.** A block is `bbN`, with its parameters and their types when it has any.
- **Instructions.** An instruction reads `value: Type = Op immediate, operands`. A terminator is written in lower case and defines no value. Any effect other than pure follows two spaces later.
- **Value names.** A value prints as its name when no other value in the function shares that name. When one does, it prints as `name.N`, where `N` is the value's id. A value with no name prints as `%N`.
- **Types.** Types print as `MidoriType::ToString()` does, so a declared type carries its module: `Shapes::Circle`.
- **Constants.** Text constants are quoted, with `\"`, `\\`, `\n`, `\t`, `\r` and `\xNN` escapes. A Float always shows a point or an exponent.

## Lowering

`Lowering` (`compiler/src/Compiler/Lowering/`) turns the checked AST of one module into a `LoweredModule`: its MidoriIR, and what the IR does not carry: the names it exports and imports, which the linker needs, the generics a module importing it specializes, and the native libraries its foreign functions come from. The verifier then checks the module; a violation stops it with `CompilerInternalError`. It lowers every construct the language has.

- **The top-level function.** The module's top-level statements become the function `$main$`, which returns `Unit`.
- **Top-level definitions.** Every one gets its global before any body is lowered, every `def f = fn ...` and instance method its function, every generic its template, and every class and instance its entry for resolving methods. A call of one of this module's functions is a direct `Call f` or `tailcall f`, and a call of any other global a `CallGlobal`.
- **Tail position.** A call that is the last thing a function does, through `if` branches, match arms and a block's final expression, becomes a `tailcall`, unless it is a foreign call or its result is not the function's.
- **Locals.** A local is the value its definition computed. A value that depends on the path taken, as an `if`'s or a match's does, is a parameter of the block where the paths meet.
- **`Never`.** A call of a function that returns `Never` is followed by `unreachable`, and lowering goes on in a block nothing reaches, where it makes no specialization and resolves no method.
- **Data.** Tuples, arrays, structs and unions are made and taken apart by their own instructions. A record update reads each member it keeps and makes a new struct. A newtype is its representation.
- **`match`.** The cases are tried in order, each a chain of tests that jumps to the next case at the first that fails and defines the names it binds on the way; a guard is one more test. A case that cannot fail ends the chain.
- **Loops.** `for` and array comprehensions over a range, an array or an Iterable become a loop whose header takes the position (the element, the index or the iterator) and anything carried round it: a comprehension's result grows by `ArrayAppend`. The loop variable is a value of the body, so a closure made in one iteration keeps that iteration's.
- **Closures.** A lambda captures exactly the enclosing values its body reads, including through lambdas nested in it, and reads them with `GetCapture`. A local function that names itself, or one defined after it, is made first and gets that capture from `BindCaptures` once the local is defined.
- **Generics.** A call of a generic specializes it for its argument types, and for the call's type where only the result names a type parameter, as one function per set of types. An equality constraint such as `Iterable::Item<S> ~ A` also says what `A` is, and an associated type projection becomes the type its instance binds. A method of an instance whose head names a type parameter is a generic too. A specialization of another module's generic reads that module's names, and its procedure is named for that module.
- **Classes.** A class method call, an operator a class provides and a `for` over an Iterable resolve to an instance method through `InstanceResolver`, which the code generator also calls, by the argument's own type: converting a newtype to its representation keeps the newtype for this.
- **Foreign functions.** A foreign function's global, or local, holds the name the VM looks it up by. A call of a builtin is a `CallForeign` by its name, a call of any other one a `CallForeign` of the name its declaration holds. A foreign function used as a value is a small function that calls it.

A diagnostic lowering reports is under the `Lowering` stage: an unresolved or ambiguous method, an unknown builtin, an unsupported foreign return type, a generic function used as a value, or an out-of-range literal.

## Bytecode backend

`BytecodeBackend` (`compiler/src/Compiler/BytecodeBackend/`) turns a `LoweredModule` into the same `BytecodeModule` the code generator produces, so the linker takes either. `$main$` is procedure 0 and every other function follows in order, named `name@Module`. Imported globals become the placeholders the linker resolves. A block nothing reaches is not emitted.

Each value lives in one of four places:

- **A frame slot.** A function's parameters are its first slots. A block parameter, and a value used more than once or in another block, is stored in a slot when it is made and read at each use. Two values share a slot when neither is live where the other is defined, from liveness over the function's blocks.
- **The operand stack.** A value used once, in the block that makes it, stays where its instruction left it, if its use takes it from there: its use's other stack operands come after it, and nothing made between is left above it.
- **Nowhere, pushed again at each use.** A constant other than Text, and every `Unit` value.
- **Nowhere at all.** A value nothing uses is popped as soon as it is made.

A jump pushes all of its arguments before it stores any, so passing a block's parameters back to it in another order is safe.

The backend chooses these superinstructions: a comparison of Ints or Floats that a `branch` takes straight from it becomes one compare-and-jump opcode, `IF_LOCAL_LE_INT` when it compares a local with a small constant and `IF_LOCAL_GE_LOCAL` when it compares two locals; `SubInt` of a local and a small constant is `PUSH_LOCAL_SUB_INT`; two locals loaded together are `GET_LOCAL2`. Cell storage is known before emission, so nothing is rewritten after it is emitted.

Procedures and globals are named in two bytes (`CALL_PROC_WIDE`, `MAKE_FUNCTION_WIDE`, `MAKE_CLOSURE_OF` and the `_WIDE` global forms), because the linker adds each module's first procedure and global to them and a program may have more than 256 of either. A closure with captures is `MAKE_CLOSURE_OF`, which boxes each capture in a fresh cell, with `Unit` for the ones `SET_CAPTURE` fills later; a union field is `GET_UNION_FIELD`. The backend's only diagnostics are the bytecode encoding's limits, as `CodeGeneratorLimitExceeded`.

## Command line

Both options are hidden from `marmotc --help`, and both go away once MidoriIR is the only path.

- **`--backend ast|ir`** is accepted by `marmotc check` and `marmotc build`. It picks the path after static analysis: `ast` (the default) runs the AST optimizer and the code generator, and `ir` runs lowering, the MidoriIR optimizer and the backend. There is no MidoriIR optimizer yet. `python scripts/testing/language.py --backend ir` runs the language suite through it, which the gate does as its `language-ir` step, and `test/midori_ir/` holds the tests written for it. A test the code generator rejects but MidoriIR lowers has a `.ir.expected` beside it, with what it prints through MidoriIR.
- **`--emit-ir`** prints each module's MidoriIR, after optimization, in the textual form above, in link order. It needs `--backend ir` and cannot be combined with `--format json`.
