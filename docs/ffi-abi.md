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

- `MidoriFFIRegistry::ABI_VERSION == 1`
- `MIDORI_FFI_MAX_ARITY == 4`

Call contract:

- `args` points to the argument slots for the current call
- only the first `arity` slots are meaningful
- `ret` points to storage owned by the Marmot VM for the return value
- native code must not keep `args` or `ret` pointers after the call returns

## Argument Marshalling

The runtime uses `FFIArgumentKind` to describe how arguments are presented to
native code.

### `RawValue`

- The slot contains the raw Marmot value bits for scalar values.
- This is the lowest-level form and assumes the callee understands Marmot's
  in-memory value representation.

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

- `data` points at the array's element storage.
- The view is borrowed; native code must not free it.

### `TraceableHandle`

- The slot contains a `MidoriTraceable*` managed by the Marmot GC.
- The handle is borrowed and only valid during the call.

### `ValueHandle`

- The slot contains `MidoriValue*` pointing at a VM-owned value.
- The pointee is borrowed and only valid during the call.

Manifest-driven dynamic FFI currently uses the same physical call signature, but
does not yet expose per-function `FFIArgumentKind` metadata in `package.marmot`.
The runtime therefore uses the generic dynamic-call marshalling path for those
functions.

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
- `FFIArray::data` must point to a `MidoriValue*` buffer.
- Marmot takes ownership of that buffer, wraps it as a Marmot array, and frees
  the outer `FFIArray` wrapper.

### `ArrayStrings`

- Native code stores an `FFIArray*` in `ret`.
- `FFIArray::data` must point to `char**`.
- Marmot copies each string into managed `Text`, frees each `char*`, then frees
  the string pointer array and the outer `FFIArray` wrapper.

### `Value`

- Native code writes a `MidoriValue` into `ret`.
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

- The current Marmot runtime is single-threaded at the VM boundary.
- Native calls run on the VM thread.
- No concurrent re-entry guarantee is provided for FFI packages in ABI v1.

## Load-Time Validation

The `marmot` tool rejects a package whose `abi_version` does not match the
runtime's. Before a program runs, `marmotc` validates:

- each native library the program names can be found and loaded
- every symbol the program declares from it exists in the loaded library
- selected prebuilt library checksums match when provided

These checks happen before user code can call the package's native functions.

## Native Failures

Current behavior:

- ABI mismatch or missing symbols fail package load with a structured module
  error
- access violations and divide-by-zero faults still bubble to the VM's top-level
  platform fault handlers and surface as runtime panics

Not yet implemented in ABI v1:

- per-call native crash wrapping with a package/function-specific panic message
- a recovery contract for native code after a fault
