# Current `.mmc` wire format (under development)

**Status: Under development; not sealed.** There is no released `.mmc` format
version. The header currently carries numeric marker `22` so the VM can reject
artifacts written for a different encoding. That marker is an internal
compatibility check, not a Marmot or `.mmc` release version. This file and its
fixtures record current compiler/VM behavior and may change as Marmot develops.

This records the bytes written by the current `marmotc` and read by the current
`marmotvm` for that format marker. It is a snapshot for keeping their
implementations in sync, not a promise to preserve this encoding or read
artifacts from older toolchains. All multibyte integers in the artifact
container are little-endian. Offsets below are byte offsets from the beginning
of the file.

## Header

| Offset | Size | Field | Current meaning |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MBC` followed by `00` |
| 4 | 4 | format marker (`u32`) | Exactly `22` |
| 8 | 2 | Marmot major version (`u16`) | Written from the producer version; informational to the current reader |
| 10 | 2 | Marmot minor version (`u16`) | Informational |
| 12 | 2 | Marmot patch version (`u16`) | Informational |
| 14 | 2 | reserved (`u16`) | Writer emits zero; current reader does not validate it |
| 16 | 4 | flags (`u32`) | Bit 0 means embedded source files follow the native libraries |
| 20 | 8 | payload size (`u64`) | Number of bytes immediately after the 32-byte header |
| 28 | 4 | payload CRC32 (`u32`) | CRC-32/IEEE of exactly those payload bytes; polynomial `0xEDB88320`, initial and final XOR `0xFFFFFFFF` |

The current reader checks magic, format marker, declared payload length, and CRC before decoding the payload. It rejects truncated fields while decoding, a procedure whose line-info byte counts do not sum to its bytecode byte count, a shape table that does not begin with shapes 0 and 1, and a frame map whose captures' shape is not in the table or whose safepoints are not in ascending offset order within the bytecode. It also rejects referenced global indices outside the global table or out of strictly ascending order. It does not validate the informational producer version, reserved field, unknown flag bits, trailing file bytes, or bytes remaining after the parsed payload. These are observations of the current encoding, not permissions for new writers to add data silently.

## Payload

`string` below means a `u32` byte length followed by that many bytes. The writer stores the bytes of its C++ strings; source paths and Marmot text are UTF-8 in normal compiler output. All counts are `u32`.

| Order | Field | Encoding |
|---:|---|---|
| 1 | entry source file | `string` |
| 2 | string pool | count, then that many `string` values |
| 3 | globals | count, then for each a name `string` and a kind `u8` |
| 4 | shapes | count, then for each a pointer-field count and that many `u32` field indexes |
| 5 | procedures | count, then the records below in index order |
| 6 | native libraries | count, then the records below in index order |
| 7 | embedded source files | Present only when header flag bit 0 is set; count, then the records below |

A kind is `1` for a value that is a heap object and `0` for one that is not. Nothing in the VM decides it from a value's bits. A shape lists the fields of a tuple, struct, union or closure that hold pointers, in ascending order. Shape 0 lists none and shape 1 only field 0, in every program; the reader rejects a table that does not begin with those two.

Each procedure is: name `string`; source-path `string`; bytecode byte count `u32`; exactly that many raw bytecode bytes; line-info count `u32`; then `(source line i32, byte count i32)` pairs; then its frame map and `referenced_globals` table. Line counts describe consecutive **bytes** in the bytecode stream, including operand bytes, not a count of logical instructions.

The frame map starts with the slot count (`u32`) and one kind (`u8`) per slot,
with parameters first. Each slot has one kind throughout the procedure.
Next comes the captures' shape (`u16`), shared by every closure of that
procedure. The safepoint count (`u32`) follows, then the entries in increasing
offset order.

Each entry starts with the bytecode offset (`u32`) immediately after the
instruction. Then comes the temporary count (`u32`) and one kind (`u8`) per
temporary, starting at the bottom of the frame's operand stack. The final
`u8` gives the kind of the result the instruction pushes.

For a non-tail Marmot call, the entry describes the caller while the callee
runs and after the result is pushed. `CONCAT_ARRAY`, `CONCAT_TEXT` and
`MAKE_CELL` have entries for collection after they push their results. The
writer omits entries for intrinsic and foreign calls. At a `JUMP_BACK` target the frame has
only its slots. At the callee's entry after `TAIL_CALL` it has only parameters.
The slot map is enough in both cases.

The `referenced_globals` table follows the frame map. It starts with a `u32`
count followed by that many `u16` global indices in strictly ascending order.
It lists globals read by `GET_GLOBAL_WIDE` or called by `CALL_GLOBAL_WIDE`
in the procedure and in any procedure reachable through `CALL_PROC_WIDE`,
`MAKE_FUNCTION_WIDE`, or `MAKE_CLOSURE_OF`. Worker transfer copies the values
of those globals and follows the same tables for closures found in those values.

Procedure names use `name@module`. The bootstrap name begins with
`$module_bootstrap$` and the main name with `$main$`.

Each native-library record is: library-name `string`; symbol count and symbol-name strings; hint-directory count and directory-path strings; `thread_safe` `u8`; `has_checksum` `u8`; and a checksum `string` when `has_checksum` is nonzero. The current reader treats any nonzero boolean byte as true. Hint directories are serialized source-derived paths, not an address or pointer to a loaded library.

Each embedded-source record is: source-path `string`; line count `u32`; then source-line strings split at LF. The LF is omitted; a preceding CR from CRLF input can remain in the stored line. The writer includes distinct procedure source paths for which it has source lines, in first-seen procedure order. Source files are optional; line-info and procedure source paths are present even when sources are not embedded.

## Instruction and intrinsic IDs

[`registry.json`](registry.json) records every opcode's ID, total encoded length, operands, and stack effect, along with intrinsic IDs and signatures. These values describe the current encoding and may change as Marmot develops. The one-byte opcode and intrinsic IDs are their zero-based positions in their respective lists.

An instruction starts with its opcode byte, followed by the `operands` in the listed order. Each operand has a `name` and an `encoding`. `u8`, `u16le`, `u24le`, and `u64le` are unsigned integers. `i8` and `i64le` are signed two's-complement integers, and `f64le` is IEEE-754 binary64. All multibyte operands are little-endian. The registry's `length` includes the opcode byte.

`stack_before` and `stack_after` list the affected part of the value stack from bottom to top. Each entry gives a slot's `name` and `type`. An optional `count` gives the number of successive slots in source order. It is an integer, the name of an operand, or `intrinsic_arity` from the selected intrinsic's signature. Without `count`, the entry describes one slot. `Value` is a slot whose type depends on the program. `T` stands for a type shared by the entries that use it. For `BITWISE_*`, `T` is `Int`, `Byte`, or `Word`. An empty list describes zero slots on that side of the operation. The prefix below the listed slots stays in place for ordinary instructions.

Non-tail calls show the caller's stack before the call and after it returns. `TAIL_CALL` replaces the current frame and enters the target with the listed parameters. `RETURN` discards the callee frame and puts its result on the caller's stack. `HALT` ends execution with the stack as it stands. `PUSH_PLACEHOLDER` reserves the local slots after the parameters. `SET_LOCAL` and `SET_GLOBAL_WIDE` copy the top value into the named slot, while `STORE_LOCAL` and `DEFINE_GLOBAL_WIDE` pop it. `GET_CELL` and `GET_CELL_WIDE` read captures from the running closure. `READ_CELL` reads the value in a `Ref` from the stack.

Jump offsets count from the byte after the instruction. `JUMP_BACK` subtracts its offset and forward jumps add theirs. `JUMP_IF_FALSE` peeks at its condition. The `IF_INTEGER_*` and `IF_FLOAT_*` instructions pop both operands and jump when the named comparison is false. Fused local instructions read directly from local slots. Their side effects appear in the registry's `description` field.

The registry generates each project's private `OpCodes.def` and `Intrinsics.def` with `python scripts/dev.py generate-mmc`. Generated files are checked in, so either CMake project can build without Python or the other project. `python scripts/dev.py generate-mmc --check` detects stale outputs without writing them. The `.mmc` gate checks generated-file freshness and agreement with the current registry, then exercises the current fixtures. Update the registry, generated files, format marker, and fixtures together when changing the encoding.

`CALL_INTRINSIC` has two one-byte operands. The first is the intrinsic ID, whose signature gives the number of arguments. The second is 1 for array results holding VM pointers and 0 otherwise. The instruction replaces its arguments with one result.

Native foreign calls use the native-library records and a symbol name. `CALL_FOREIGN` encodes each argument in two bits starting at bit 0. Kind `0` passes raw bits, `1` passes a C string for `Text`, `2` passes an array of scalar slots, and `3` passes an array of C strings for `Array<Text>`. Return kind `0` carries raw bits, `1` a C string, `2` an array of values, and `3` an array of C strings. Bit 7 marks arrays of VM pointers for the collector, including managed texts. The [native extension ABI](../ffi/ABI.md) defines the separate calling and ownership contract.

Each value has a kind and each aggregate a shape, so no operation has to decide from a value's bits whether it is a pointer. Instructions that build objects or send values between workers carry the needed metadata: `CREATE_ARRAY` its elements' kind; `CREATE_TUPLE`, `CONSTRUCT_STRUCT`, `CONSTRUCT_UNION` and `LOCAL_UNION2` a shape, low byte first, as their last two operands; `MAKE_CELL` and `CHANNEL_SEND` the value's kind; `SPAWN_WORKER` the shape of the tuple its arguments and function cross in, then the kind of the worker's result; `CHANNEL_RECEIVE` the kind of its option payload; and `JOIN_WORKER` the kind of the worker's value. A closure takes its procedure's captures' shape.

`JOIN_WORKER` produces an outcome union with tag 0 holding the worker value and tag 1 holding an error union. In the error union, tag 0 is cancellation with no payload and tag 1 is failure with a `Text` message. `CHANNEL_RECEIVE` produces tag 0 with no payload for a closed and drained channel, or tag 1 holding a message. The compiler checks the declared primitive signatures against these layouts. The prelude maps these outcomes to public unions using ordinary Marmot constructors.

Native library records carry names, symbols, lookup hints, concurrency policy, and an optional checksum. The VM resolves these imports at run time. See the [standalone executable format](../standalone/format.md) for bundles that carry library files alongside the bytecode.

## Marker coordination

The format marker identifies the encoding expected by the reader. When a change
could make an artifact decode or execute differently, change this marker so a
reader built for another encoding rejects it rather than misinterpreting it.
There is no requirement to preserve old opcode or intrinsic IDs, layouts, or
artifacts. The compiler and VM built together must use the same registry. The
producer's major/minor/patch header fields do not replace the marker check.

[`fixtures.json`](fixtures.json) records the compiler commit, source files, path normalization, expected VM behavior, and derivation of invalid artifacts for the current format. The committed fixture files deliberately use repository-relative paths so they are portable. The conformance check recompiles their sources, normalizes only serialized checkout paths, compares all remaining bytes, and runs the VM on these regression fixtures. Update them when a deliberate format change changes the current output.

Run the check with `python scripts/testing/mmc_contract.py`; it also runs as the `mmc` step of `python scripts/dev.py gate`.
