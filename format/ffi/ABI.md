# Marmot native extension ABI

An extension uses the target platform's C ABI and the declarations in
[`MarmotFFI.h`](include/marmot/MarmotFFI.h). It must match the VM's
architecture. The supported targets have 64-bit pointers and IEEE 754 binary64
`double`, including the browser bundle's wasm64 target.

## Exports and loading

Define `MARMOT_FFI_IMPLEMENTATION` before including the header in the source
that exports the result deallocator.

```c
void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr);
```

Export each callable symbol with `MARMOT_FFI_EXPORT` and `MARMOT_FFI_CALL`.
Use `extern "C"` for its definition in C++. Exceptions must stay inside the
library. The VM requires the deallocator and resolves callable symbols by
the logical library name and the symbol declared in Marmot.

```marmot
foreign "marmot_example_count" Count: fn(Array<Text>) -> Int from "marmot_ffi_example";
```

Run with the Marmot tool or pass
`--library marmot_ffi_example=<shared-library-file>` to `marmotvm run`.
A browser host supplies the same exports through its bundled library resolver.

## Call storage

A callable symbol has type `MarmotFFIFn`, taking `void** args` and `void* ret`
and returning `void`. Only the first `arity` slots are valid, up to four.
Each scalar occupies eight bytes in host byte order. Copy a scalar from
`&args[i]` with `memcpy`. Write its result into `ret`, which points to an
eight-byte slot owned by the VM.

| Marmot type | Slot contents |
| --- | --- |
| `Int` | Signed 64-bit integer |
| `Word` | Unsigned 64-bit integer |
| `Float` | IEEE 754 binary64 |
| `Bool` | Byte value 0 or 1 at offset 0, with the other bytes zero |
| `Byte` | Unsigned integer from 0 to 255 in a 64-bit slot |
| `Unit` | Zero |

| Argument kind | What `args[i]` holds |
| --- | --- |
| 0 | Scalar bits in the slot |
| 1 | `const char*` to NUL-terminated UTF-8 text |
| 2 | Pointer to a `MarmotFFIArray` containing eight-byte scalar slots |
| 3 | Pointer to a `MarmotFFIArray` containing `const char*` entries for `Array<Text>` |

The VM rejects embedded NUL bytes in C-string arguments, including individual
strings in `Array<Text>`, with `InvalidConversion`.

`MarmotFFIArray` contains `void* data` followed by `int32_t length`, with
the platform's natural struct padding. `length` counts elements. An empty
array may have a null data pointer. Arrays containing other Marmot objects
have no portable SDK representation. Treat text, arrays, and their contents
as read-only borrowed data. Borrowing ends when the callable symbol returns.

## Owned results

Write pointers to results into `ret` with `memcpy(ret, &result, sizeof(result))`.
The library owns all storage reachable from the returned pointer.

| Return kind | Contents |
| --- | --- |
| 0 | Eight-byte scalar slot |
| 1 | `char*` to NUL-terminated UTF-8 text |
| 2 | `MarmotFFIArray*` containing eight-byte scalar slots |
| 3 | `MarmotFFIArray*` containing `char*` to NUL-terminated UTF-8 strings |

A null result pointer represents empty text or an empty array according to
the expected type. The VM copies each non-null owned result into its own
storage, then calls that library's `MARMOT_FFI_FreeResult` once with the
return kind and the original pointer. The library releases the complete
result, including the wrapper, data buffer, and any individually allocated
strings. The VM keeps the library loaded through this call. A deallocator
may use any allocator or storage strategy the library supports.

## Build the example

```sh
cmake -S example -B build
cmake --build build
marmotc build example/Main.mmt -o example.mmc
marmotvm run example.mmc --library marmot_ffi_example=<built-library-file>
```

The program exits successfully when the extension receives the text array and
the VM copies the returned string.
