# FFI ABI

Marmot currently exposes `FFI ABI v1`.

Dynamic native packages must declare the ABI they target in `package.marmot`:

```toml
[ffi]
enabled = true
library_name = "marmot_image"
abi_version = 1
```

The package's source declares the functions it uses, naming the library:

```marmot
foreign "marmot_image_read_info" ReadInfo : fn(Text) -> Int from "marmot_image";
```

If `abi_version` does not match the runtime's current ABI version, manifest load
fails before the library is used.

## Calling Convention

All native entry points use this signature:

```cpp
using FFIFunction = void(*)(void** args, void* ret);
```

Related runtime constants:

- `VmBuiltins::ABI_VERSION == 1`
- `MARMOT_FFI_MAX_ARITY == 4`

Call contract:

- `args` points to the argument slots for the current call
- only the first `arity` slots are meaningful
- `ret` points to storage owned by the Marmot VM for the return value
- native code must not keep `args` or `ret` pointers after the call returns

## Argument Marshalling

The runtime uses `FFIArgumentKind` to describe how arguments are presented to
native code.

### `RawValue`

- The slot holds the scalar's raw bits. Copy them from `&args[i]`. In a
  scalar array, each element's 8-byte slot holds those same bits.

### `CString`

- The slot contains `const char*` UTF-8 text.
- The pointer is borrowed for the duration of the call only.
- Native code must not free or retain it.

### `ArrayView`

- The slot contains a pointer to:

```cpp
struct FFIArray
{
    void* data;
    int length;
};
```

- `data` points at the array's elements, one 8-byte slot of raw value bits
  each, in every build of the VM.
- Treat the borrowed view and its element buffer as read-only. For non-scalar
  elements the slots hold VM-internal handles, including for `Array<Text>`.
  An empty array can have a null data pointer.

### `TraceableHandle`

- The slot holds a `MarmotTraceable*` managed by the GC. Builtins borrow this
  handle for the duration of the call.

### `ValueHandle`

- The slot holds a `MarmotValue*` pointing to a VM-owned value. Builtins
  borrow it for the duration of the call.

A native library's function gets its kinds from its `foreign` declaration's
types, so `package.marmot` declares none: a `Text` parameter is passed as
`CString`, an `Array<T>` as `ArrayView`, and any other as `RawValue`. A `Text`
result is taken as `CString`, an `Array<Text>` as `ArrayStrings`, any other
array as `ArrayValues`, and anything else as `RawValue`. An array a native
function returns holds texts or scalars: the compiler rejects a declaration
that returns an array of other values, whose elements the VM would have to
take as pointers. The compiler rejects a
call to a native function with more than `MARMOT_FFI_MAX_ARITY` arguments.

## Return Value Marshalling

The runtime uses `FFIReturnKind` to describe how `ret` is interpreted.

### `RawValue`

- Native code writes a Marmot-compatible raw value into `ret`.
- Ownership remains with the VM.

### `CString`

- Native code stores a `char*` in `ret`.
- Marmot copies the text into managed `Text` storage and then frees the native
  buffer with `std::free`.
- Return buffers must therefore be allocated with a `malloc`-compatible
  allocator that is safe to free from the Marmot process.

### `ArrayValues`

- Native code stores an `FFIArray*` in `ret`.
- `FFIArray::data` must point to a `malloc`ed buffer of 8-byte slots, one per
  element, holding raw value bits.
- Marmot takes ownership of the buffer and frees the outer `FFIArray` wrapper.
  The VM copies small arrays into inline storage. In Debug it widens the
  ABI's element slots to tagged VM values before wrapping the result.

### `ArrayStrings`

- Native code stores an `FFIArray*` in `ret`.
- `FFIArray::data` must point to `char**`.
- Marmot copies each string into managed `Text`, frees each `char*`, then frees
  the string pointer array and the outer `FFIArray` wrapper.

### `Value`

- A builtin writes a `MarmotValue` into `ret`.
- Ownership remains with the VM unless the returned value itself refers to
  native memory described by another return kind.

## Ownership Rules

- Incoming arguments are borrowed.
- Returned `CString`, `ArrayValues`, and `ArrayStrings` transfer ownership to
  Marmot.
- Returned buffers must be allocated in a way that is compatible with
  `std::free` in the current process.
- Native code must not retain raw pointers into Marmot-managed arrays, texts, or
  traceables after the call returns.

## Threading

- A native call runs on the calling VM's thread. Workers have separate VMs
  and can call the same library at the same time.
- Libraries default to `thread_safe = false`. A worker fails before executing
  if any native library imported by the program has that setting. Libraries
  marked `thread_safe = true` must synchronize access to shared mutable state.

## Load-Time Validation

The `marmot` tool rejects a package whose `abi_version` does not match the
runtime's. Before a program runs, `marmotvm` validates:

- each native library the program names can be found and loaded
- every symbol the program declares from it exists in the loaded library
- selected prebuilt library checksums match when provided

These checks happen before user code can call the package's native functions.

## Native Failures

Current behavior:

- An ABI mismatch is reported during package manifest validation. A missing
  symbol is reported when the VM loads the library, before program execution.
- access violations and divide-by-zero faults still bubble to the VM's top-level
  platform fault handlers and surface as runtime panics

Not yet implemented in ABI v1:

- per-call native crash wrapping with a package/function-specific panic message
- a recovery contract for native code after a fault
