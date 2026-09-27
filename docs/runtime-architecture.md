# Runtime Architecture

Marmot runs programs on a single `VirtualMachine` execution path.

## Execution Flow

1. The compiler produces one `MidoriExecutable`.
2. Native and WASM entry points construct `VirtualMachine(std::move(executable))`.
3. The VM executes the bootstrap procedure, module initializers, and user code through `Execute()`.

There is no secondary runtime scheduler, worker pool, or alternate execution mode.

## VirtualMachine

`VirtualMachine` owns:

- The value stack and call stack.
- The executable and global variable array for the running program.
- The allocator and generational garbage collector.
- Caches for procedure entry points, static closures, and interned literals.

## Closures and Captures

A closure's captures are `MidoriCellValue` boxes, read with `GET_CELL`. A function with no captures uses `MAKE_FUNCTION_WIDE`; a closure uses `MAKE_CLOSURE_OF`, which takes exactly the values it captures from the stack and boxes each in a fresh cell. `SET_CAPTURE` fills a capture after the closure exists, which is how local functions that name each other reach one another. Nothing is promoted in place, and locals use `GET_LOCAL*` / `SET_LOCAL*`.

Every function with no captures is one value, which `MAKE_FUNCTION_WIDE` hands out from the static closure cache; the cache is a garbage-collection root.

## Memory Model

All GC-managed objects are VM-local:

- Text
- Arrays
- Structs
- Unions
- Closures
- Cell boxes
- Range objects

Every GC-managed object is one `MidoriTraceable`, exactly one fixed-size
allocator slot. `MidoriValue` is an untagged word in release builds, so root
and child pointer identification is conservative (region/slot-range check
plus a live-bit test): a scalar whose bits happen to alias a live slot only
over-retains that object, it is never dereferenced incorrectly. This rules
out moving/copying collection — Marmot's collector is strictly non-moving.

### Allocator

`MidoriAllocator` carves fixed 80-byte slots out of 64 KB blocks:

- Native: blocks are carved from one contiguous reserved virtual-memory
  region. On POSIX the region is committed 2 MB at a time, aligned so that
  every granule, the first included, can be backed by a transparent huge
  page; a growing heap would otherwise take one page fault per 4 KB.
- Emscripten/WASM: blocks are individually `malloc`-backed and appended to a
  block table (never sorted — slot indices must stay stable for the
  lifetime of the allocator, since the generational collector keys
  persistent bitmaps by slot index).
- A `uint64_t` live-bitmap tracks slot occupancy, one bit per slot, parallel
  across both platforms. `TryGetSlotIndex` maps a pointer to its global slot
  index; `SlotAt` is the inverse. Allocation pops from a free list; freeing
  clears the live bit and pushes back onto the free list.
- The allocator only ever hands out slots. The buffers behind long `Text`,
  `Array`, and `Tuple` payloads come from the value buffer pool in `common`
  and are never traced or treated as roots — only their owning
  `MidoriTraceable` is.

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
- **Write barrier**: storing a value into an existing object logs that
  object into the remembered set if it is old (deduplicated via a parallel
  "logged" bitmap). The barrier fires at every container-mutation opcode —
  array element/append/extend, cell writes, struct member writes, and FFI
  array-argument marshalling — so old→young pointer edges created by
  mutation are still traced on the next minor collection.
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

The collector traces values reachable from the VM stacks, globals, closure
environments, the remembered set, and nested aggregate objects.

## Globals

Global variables live in the executable's global array and are accessed through:

- `DEFINE_GLOBAL_WIDE`
- `GET_GLOBAL_WIDE`
- `SET_GLOBAL_WIDE`
- `CALL_GLOBAL_WIDE`

The runtime does not maintain shared-global indirection. A spawned worker gets
its own copy of the spawning VM's globals, taken at the spawn; see
`docs/multicore-runtime.md`.
