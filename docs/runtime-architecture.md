# Runtime Architecture

Marmot runs bytecode in isolated `VirtualMachine` instances.

## Execution Flow

1. The compiler links a `MarmotExecutable` and writes it to a `.mmc` file.
2. Native and WebAssembly entry points decode the `.mmc` into a `VmExecutable`
   and pass it to `VirtualMachine`.
3. The main VM executes the bootstrap procedure, module initializers, and user code.
   Native-thread mode calls `Execute()` directly. Cooperative mode drives the
   main VM and its workers through `ExecuteSlice()`, which calls `Execute()` and
   preserves standard-library error state across slices.

`ExecuteLoop()` selects a compiled specialization of `ExecuteInstructions` at
entry. Both specializations share opcode implementations. The cooperative
specialization adds scheduling checkpoints at calls, tail calls, returns, and
loop back-edges. When a VM yields or parks, it saves its instruction and stack
state so the scheduler can resume it on its assigned thread. Native-thread
dispatch omits the scheduler checks. See [Multicore Runtime](multicore-runtime.md)
for scheduling and blocking behavior.

## VirtualMachine

`VirtualMachine` owns:

- The value stack and call stack.
- A shared reference to the immutable executable and its own global variable array.
- The allocator and generational garbage collector.
- Caches for procedure entry points, static closures, and interned literals.

## Closures and Captures

A closure holds its captured values themselves, read with `GET_CELL`. A function with no captures uses `MAKE_FUNCTION_WIDE`; a closure uses `MAKE_CLOSURE_OF`, which takes exactly the values it captures from the stack. `SET_CAPTURE` fills a capture after the closure exists, which is how local functions that name each other reach one another. Nothing is promoted in place, and locals use `GET_LOCAL*` / `SET_LOCAL*`.

Every function with no captures is one value, which `MAKE_FUNCTION_WIDE` hands out from the static closure cache; the cache is a garbage-collection root.

## Memory Model

All GC-managed objects are VM-local:

- Text
- Arrays
- Structs
- Unions
- Closures
- References (`Ref<T>`)
- Range objects

Every GC-managed object is one `MarmotTraceable` in one allocator slot. It
starts with an 8-byte header (its type; whether an array's or a cell's values
are pointers; a union's tag or a closure's procedure; and an aggregate's
16-bit length and 16-bit shape). Dev and Release store aggregates of up to
nine values inline. Larger aggregates use a separate buffer. Debug uses larger
tagged values, so fewer fit inline. The header can hold a length of up to
65,535. Struct and union constructors take at most 255 fields in bytecode.

`MarmotValue` is an untagged word in release builds, and nothing decides from
its bits whether it is a pointer. The compiler writes down where every pointer
is (since format 15): a kind for each global; for each procedure, a kind for each
frame slot, which holds one kind of value for the whole procedure, and the
kinds of the temporaries on its operand stack where a collection can start
with some there; and a shape for each aggregate, listing its pointer fields.
The collector reads roots and fields from those alone. It walks the frames
through the call stack, each from its base to the base of the frame it called
and the top one to the stack pointer. During execution, collection can start at
`JUMP_BACK`, `TAIL_CALL`, `RETURN`, `CONCAT_ARRAY`, `CONCAT_TEXT` and
`MAKE_CELL`. Debug builds keep a type tag in every value and check each map
against the tags as values are stored and at each of those places. The
collector is non-moving.

### Allocator

`MarmotAllocator` carves slots out of 64 KB blocks. Slots come in five sizes,
16 to 80 bytes in steps of 16. A two-field union takes 32 bytes, and text and
arrays take 80. A block keeps its slot size until all its objects are gone.
When an allocation needs a block, the allocator can take an empty one and
divide it into slots of a different size. On native builds, the allocator
keeps that memory committed until it shuts down.

- Native: blocks are carved from one contiguous reserved virtual-memory
  region. On POSIX the region is committed 2 MB at a time, aligned so that
  every granule, the first included, can be backed by a transparent huge
  page; a growing heap would otherwise take one page fault per 4 KB.
- Emscripten/WASM: blocks are individually `malloc`-backed and appended to a
  block table (never sorted — slot indices must stay stable for the
  lifetime of the allocator, since the generational collector keys
  persistent bitmaps by slot index).
- A `uint64_t` live-bitmap tracks slot occupancy, one bit per slot, parallel
  across both platforms; each block owns whole words of it, as many as its
  slots need. `TryGetSlotIndex` maps a pointer to its global slot index,
  dividing by the block's slot size with a multiply, and rejects a pointer
  into the middle of a slot; `SlotAt` is the inverse. Allocation pops from
  the free list of its size; freeing clears the live bit and pushes back
  onto that list.
- The slot allocator holds the `MarmotTraceable` objects. If one needs an
  external buffer, it uses the value buffer pool when the buffer fits a pool
  class and `malloc` for larger buffers. The collector follows pointers in
  the buffer through the object that owns it.

### Garbage Collector

`GarbageCollector` is a bitmap mark-sweep collector with generational
(sticky-mark) collection on top:

- **Mark bits** are a `uint64_t` bitmap parallel to the allocator's live
  bitmap — no per-object mark flag, no separate list of live objects.
  Marking is `TryGetSlotIndex` → live-bit test → mark-bit test-and-set.
- **Sweep** walks the bitmap word by word, computing `live & ~mark` per word
  and destroying/reclaiming any set bit; it never inspects individual
  objects that survive.
- **Minor collections** (the default trigger) do not clear mark bits first.
  A mark bit that survived a previous cycle means "old": old objects
  terminate tracing immediately and are never re-visited. Only young
  (unmarked) objects are traced from the VM roots and the remembered set;
  young survivors keep their new mark bit, which is promotion to old.
- **Write barrier**. The VM adds old objects to the remembered set when it
  appends to or extends an array, fills a late closure capture, writes to a
  `Ref<T>`, or passes an array argument to FFI. A bitmap keeps the same
  object from being added twice. The next minor collection checks these
  objects for pointers to young ones.
- **Major collections** clear all mark bits and the remembered set, then
  trace and sweep the whole heap. They run at VM shutdown (`force_clean`)
  and whenever live bytes after a minor collection exceed 2× live bytes
  after the last major, or 4× when that major reclaimed less than a quarter
  of the heap: a heap that only grows would otherwise be re-traced in full
  every time it doubled.
- Collection triggers on a byte-allocated threshold (`ShouldCollect`),
  starting at `INITIAL_GC_THRESHOLD`. After each cycle the next threshold is
  the live bytes plus headroom of half the live bytes, with the headroom
  capped at `MAX_GC_HEADROOM` and the threshold never below
  `MIN_GC_THRESHOLD`. The cap bounds the growth, never the threshold itself,
  so the threshold always stays above the live bytes; a threshold at or below
  them would make every allocation check run a collection that frees nothing.
  The VM checks the threshold at the safepoints above. If a program crosses
  it between checks, allocations can continue until the next safepoint.

The collector traces values reachable from the VM stacks, globals, closure
environments, the remembered set, and nested aggregate objects, each as its
map says.

## Globals

The executable records the global slots. Each VM holds the values in its own
global array, accessed through:

- `DEFINE_GLOBAL_WIDE`
- `GET_GLOBAL_WIDE`
- `SET_GLOBAL_WIDE`
- `CALL_GLOBAL_WIDE`

The runtime does not maintain shared-global indirection. A spawned worker gets
its own copy of each global its code can read, taken at the spawn; see
`docs/multicore-runtime.md`.
