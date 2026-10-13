# Precise values: no pointer guessing

Branch `precise-values`. Goal: nothing in Marmot decides whether eight bytes are
a pointer by looking at them. Today the VM asks the allocator whether the bits
name a live slot (`GarbageCollector::Contains`, `MidoriAllocator::Contains`,
`TryGetSlotIndex` on arbitrary bits) in six places:

1. the collector's root scan of the value stack;
2. the same scan of the globals;
3. the collector's trace of every array element, aggregate field and cell;
4. value transfer between workers (`IsHeapObject`);
5. `PassForeignArgument`, though its argument kinds are typed since format 14;
6. the current closure and each frame's closure, which are always pointers.

A guess in 1-3 keeps a dead object alive. A guess in 4 changes a number that
crosses to a worker. Both go: the compiler says where every pointer is, and the
VM follows what it says.

## Decisions (2026-10-13)

- **Stack roots: compiler stack maps.** No per-push tag. The backend records
  which frame slots hold pointers and, where a collection can start with
  temporaries on the operand stack, which of those are pointers.
- **Object maps: 16-bit length plus 16-bit shape id.** The header stays 8 bytes.
  An aggregate (tuple, struct, union, closure) holds at most 65,535 values. Arrays
  and cells use the spare header byte as "holds pointers".
- **Workflow:** this branch, one commit per phase, merged once the gate passes on
  Dev and Debug.

## What the compiler knows and must write

One function decides whether a type is a pointer at run time: Text, Array, Tuple,
Struct, Union, Function, Ref and Range are; Int, Float, Byte, Word, Bool, Unit,
Never, Channel and Worker are not; a newtype is what it wraps. A type variable or
generic parameter reaching the backend is a compiler bug.

`.mmc` format 15 adds:

- **Globals:** a kind byte per global.
- **Shapes:** a table of pointer-field lists. Shape 0 has no pointer fields, so
  it serves an aggregate of any length whose fields are all scalars.
- **Procedures:** each record gains its frame's slot count and slot pointer map,
  its captures' shape id, and a safepoint table: for each bytecode offset at
  which a collection can find the frame with temporaries on its operand stack,
  how many temporaries there are and which are pointers, and whether a call's
  result, pushed on top, is one.
- **Operands:** a shape id on every instruction that builds a tuple, struct or
  union (`CREATE_TUPLE`, `CONSTRUCT_STRUCT`, `CONSTRUCT_UNION`, `LOCAL_UNION2`);
  a kind on `CREATE_ARRAY`, `MAKE_CELL` and `CHANNEL_SEND`; the crossing tuple's
  shape and the result's kind on `SPAWN_WORKER` and `Ok`'s value kind on
  `JOIN_WORKER`, for the values the VM builds and copies; and bit 7 of a foreign
  call's return tag for an array whose elements are pointers. A payload records
  its root's kind, so `CHANNEL_RECEIVE` needs none, and a closure takes its
  procedure's captures' shape.

Slots are shared only between values of the same kind, so a slot's kind holds for
the whole procedure. A slot starts as the zero the frame's placeholders push, and
a pointer slot only ever holds pointers after that, so a dead pointer in it is a
real object the collector kept alive, never stale bits.

## When the collector looks at a frame

A collection starts only at `JUMP_BACK`, `TAIL_CALL`, `RETURN`, `CONCAT_ARRAY`,
`CONCAT_TEXT` and `MAKE_CELL`. Every frame below the top is suspended at a call,
its region running from its base to the callee's base. The top frame's region
runs to the stack pointer. A frame's procedure is found from its instruction
pointer: the bytecode streams are separate, so each address lies in one.

- Below a call: the caller's slots, then the temporaries below the call's
  arguments, as the call's safepoint entry lists them.
- After `RETURN`: the same, then the result on top.
- After `CONCAT_*` or `MAKE_CELL`: the temporaries below its operands, then its
  result.
- At a loop head after `JUMP_BACK`: slots only; no value stays on the operand
  stack from one block to the next.
- At a callee's entry after `TAIL_CALL`: its parameters only, before its
  placeholders are pushed.

A region longer than the slots and the entry's temporaries say is a compiler
bug.

## Checking it

Debug builds keep a type tag in every value. On every collection in Debug, each
root and field the maps call a pointer must carry a pointer tag, and each one
they call a scalar must not. The precise collector is switched on only once the
whole suite, run in Debug, agrees with the maps.

## Phases

1. **Maps, checked.** The compiler writes format 15, and the VM reads it, gives
   every object its shape or flag, and in Debug checks the maps on every
   collection while still tracing as before. Done when the gate passes on Dev and
   on Debug with every check on.
2. **Precise collector.** Roots and fields are traced from the maps alone; the
   collector no longer calls `Contains`.
3. **Native calls.** `PassForeignArgument` trusts its kinds, a native function
   whose declaration returns a pointer other than `Text` or `Array<Text>` is a
   compile error (it would hand the collector raw bits as pointers), and
   `Contains` is deleted.
4. **Close.** `runtime-architecture.md`, `multicore-runtime.md`, `ffi-abi.md` and
   `format/mmc/v15.md` describe the precise design; benchmarks compared with
   `main`; this file removed on merge.

## Progress

- 2026-10-13: plan written; branch created.
- 2026-10-13: phase 1 done. Format 15 carries the maps; every object has its
  shape or flag; Debug checks every map against value tags on each object build
  and write, each global definition and each of the six safepoints, and the
  whole suite agrees (breaking one map on purpose aborts at once). Value
  transfer moved here from phase 3: it copies by kind and shape, and no longer
  calls `Contains`.
