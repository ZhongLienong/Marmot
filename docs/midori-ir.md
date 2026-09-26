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

Every generic specialization, instance method and lambda is its own function. Every value has a concrete type, because the IR is built after monomorphization. Types are the checker's `MidoriType`.

- **Parameters.** The entry block, `bb0`, is the function's first block. Its parameters are the function's parameters.
- **Captures.** A function reads its captures with `GetCapture #i`. `m_capture_types` gives their types.
- **Block parameters instead of phi nodes.** A value that differs by the path taken is a parameter of the block where paths meet, and each jump passes it as an argument.
- **Every instruction but a terminator defines exactly one value.** An instruction run only for its effect, such as `GlobalSet`, defines a `Unit`.
- **Definitions are found from the blocks.** The value table does not record where a value is defined, so a pass that moves an instruction has nothing else to update.
- **Globals.** A global's slot is its index in the module's global table. Lowering reserves the slot of every top-level definition before it lowers any function.
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
| Bit operations | `BitAnd` `BitOr` `BitXor` `BitNot` `Shl` `Shr`, each for `Int`, `Byte` and `Word` | |
| Comparisons | `Eq` `Ne` `Lt` `Le` `Gt` `Ge` for `Int`, `Float`, `Byte` and `Word`; `EqBool` `NeBool` `NotBool`; `EqText` `NeText` | Give Bool |
| Conversions | `IntToFloat` `FloatToInt` `IntToText` `FloatToText` `WordToText` `TextToInt` `TextToFloat` `ByteToInt` `IntToByte` `ByteToWord` `WordToByte` `WordToInt` `IntToWord` `ByteToFloat` `FloatToByte` `WordToFloat` `FloatToWord` | |
| Tuples | `MakeTuple`, `TupleGet #i` | |
| Arrays | `MakeArray`, `ArrayGet array, index`, `ArrayLength` | |
| Structs | `Construct`, `GetMember #i`, `RecordUpdate #i record, value` | Members by position |
| Ranges | `MakeRange start, step, end`, `RangeStart` `RangeEnd` `RangeStep` | |
| Unions | `MakeUnion tag t`, `GetTag`, `UnionField tag t #i` | |
| Text and array growth | `Concat`, `Extend` | Both operands and the result are one type, Text or an array. `Extend` only where the left operand is provably fresh |
| Calls | `Call f`, `CallGlobal @slot`, `CallForeign foreign "name"`, `CallValue closure, ...` | Arguments follow |
| Closures | `MakeClosure f` with the captures as operands, `BindCaptures`, `GetCapture #i` | `BindCaptures` ties recursive local functions together |
| Cells | `CellNew`, `CellRead`, `CellWrite cell, value` | Only for the language's `Cell<T>` |
| Globals | `GlobalDefine @slot`, `GlobalGet @slot`, `GlobalSet @slot` | |
| Concurrency | `Spawn` `Join` `WorkerCancel` `WorkerIsDone` `ChannelNew` `Send` `Receive` `ChannelClose` | |
| Terminators | `Jump`, `Branch`, `Switch`, `Return`, `TailCall`, `Halt` | See below |

A terminator ends its block and defines no value. Its successors carry their jump arguments.

| Terminator | Meaning |
| --- | --- |
| `jump bbN(args)` | Go to `bbN` |
| `branch cond, bbT(args), bbF(args)` | Go to `bbT` when the Bool `cond` is true, else to `bbF` |
| `switch union, t: bbN(args), ..., default: bbD(args)` | Go to the case for the union's tag; the default is optional |
| `return value` | Return from the function |
| `tailcall callee, args` | Call without a new frame: a function, a global `@slot`, or, with neither, a closure given as the first operand |
| `halt` | End the program; only the top-level function uses it |

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

1. **Terminators.** Every block ends in exactly one terminator, no terminator appears anywhere else, and every function has a block.
2. **Dominance.** Every value is defined once, and every use is dominated by its definition. A use in the defining block must come after it. Uses in blocks that the entry does not reach are not checked.
3. **Successor arguments.** Every successor names an existing block. Its arguments match that block's parameters in number and type.
4. **Operand types.** Each instruction's operands, immediate and result have the types it expects: `AddInt` takes two Ints and gives an Int, a `Call` matches its callee's parameters and return type, a `Return` gives the function's return type, and so on. An instruction's type is its value's type. Only terminators have successors. The concurrency instructions, `BindCaptures` and `CallForeign` are not checked yet.
5. **Switch coverage.** A `Switch` takes a union, has at most one case per tag and at most one default, and names only tags of that union. With no default, it covers every member.
6. **Global slots.** Every `GlobalDefine`, `GlobalGet`, `GlobalSet`, `CallGlobal` and global `TailCall` names a reserved slot.

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

- **The module header.** The module's name comes first. Then come its globals by slot, as `global @0 total: Int`, and, if it has one, the function that runs its top-level statements, as `top-level Sum$top`.
- **Function headers.** Each function starts with `fn`, its name, its parameter types and its return type, followed by `captures(...)` when it has captures.
- **Blocks.** A block is `bbN`, with its parameters and their types when it has any.
- **Instructions.** An instruction reads `value: Type = Op immediate, operands`. A terminator is written in lower case and defines no value. Any effect other than pure follows two spaces later.
- **Value names.** A value prints as its name when no other value in the function shares that name. When one does, it prints as `name.N`, where `N` is the value's id. A value with no name prints as `%N`.
- **Types.** Types print as `MidoriType::ToString()` does, so a declared type carries its module: `Shapes::Circle`.
- **Constants.** Text constants are quoted, with `\"`, `\\`, `\n`, `\t`, `\r` and `\xNN` escapes. A Float always shows a point or an exponent.

## Command line

Both options are hidden from `marmotc --help`, and both go away once MidoriIR is the only path.

- **`--backend ast|ir`** is accepted by `marmotc check` and `marmotc build`. It picks the path after static analysis: `ast` (the default) runs the AST optimizer and the code generator, and `ir` runs lowering, the MidoriIR optimizer and the backend. Until lowering exists, `ir` stops every module with `CodeGeneratorUnsupportedLowering`. `python scripts/testing/language.py --backend ir` runs the language suite through it.
- **`--emit-ir`** prints each module's MidoriIR, after optimization, in the textual form above. It needs `--backend ir` and cannot be combined with `--format json`.
