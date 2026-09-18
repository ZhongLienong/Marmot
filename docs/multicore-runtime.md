# Marmot Multicore Runtime

## Execution Model: Isolated Workers

Marmot uses an **isolated-worker** concurrency model. Each worker is a full
`VirtualMachine` instance with its own allocator, garbage collector, value
stack, call stack, and global variables. Workers share read-only bytecode
(`std::shared_ptr<const MidoriExecutable>`) but cannot share mutable values.

Communication between workers happens exclusively through message passing
(typed binary-serialized values via channels).

## What Is Shared vs Isolated

| Component | Shared? | Notes |
|-----------|---------|-------|
| Bytecode (`MidoriExecutable`) | Yes (read-only) | Via `shared_ptr<const>` |
| Builtin FFI table | Yes (immutable) | `constexpr` array, compile-time |
| Dynamic FFI function pointers | Snapshot per VM | Lock-free lookup during execution |
| Shared library handles | Yes (cache) | `SharedLibraryCache` singleton, load-time only |
| Value stack | No | Per-VM, 10k slots |
| Call stack | No | Per-VM, 10k slots |
| Heap / Allocator | No | Per-VM `MidoriAllocator` |
| Garbage collector | No | Per-VM, independent generational collection |
| Global variables | No | Per-VM; a worker starts from a copy of the spawning VM's globals |
| String literal cache | No | Per-VM |
| Static closure cache | No | Per-VM |

## Value Transfer and the `Transferable` Typeclass

Cross-worker value transfer is governed by the `Transferable<T>` typeclass.
Types that satisfy `Transferable` can be deep-copied across worker boundaries
via `ValueTransfer` (direct VM-to-VM copy for `spawn`/`join`) or serialized to
`SerializedValue` (binary format for channels).

**Built-in `Transferable` instances** (deep copy):
- `Int`, `Float`, `Byte`, `Word`, `Bool`, `Unit` — bitwise copy (no heap allocation)
- `Text` — new allocation in target VM, bytes copied
- `Array<T>` where `Transferable<T>` — new allocation, elements recursively transferred
- `Channel<T>` where `Transferable<T>` — handle copy (process-wide registry index)
- `Cell<T>` where `Transferable<T>` — **copied**: the worker gets its own cell, so
  writes on either side are not seen by the other. The compiler warns
  (`CellCrossesWorker`) wherever a cell is passed to `Spawn`, captured by the
  spawned lambda, sent through a channel, or mapped with `ParallelMap`.

**Derivable for user types**:
- Structs and unions can `deriving (Transferable)` if all fields/variants satisfy `Transferable`
- Tuples are transferable if all element types satisfy `Transferable`

**Never transferable** (no `Transferable` instance):
- Ranges — semantically non-portable
- `Worker<T>` — joining from a non-owner worker is undefined

**Functions** (`fn(A) -> B`) are transferable: a function crosses as its procedure
index plus a copy of the cells it captured. What a closure captured is not part of
its type, so the captures themselves are not checked at compile time — a closure
that captured a `Worker<T>` crosses, and the handle is meaningless on the other
side. This is the one place transferability is not decided at compile time.

Transferability is checked at compile time. Attempting to spawn with a
non-transferable argument or create a `Channel<T>` where `T` lacks a
`Transferable` instance produces a constraint-failure error.

Copying a closure and its captured cells is sound because v2 values, closures
included, are immutable once built. The one exception is a `Cell<T>`, whose
contents can change; it is copied like everything else, which is why crossing
with one is a warning rather than a data race. The same copy carries a worker's
globals, the spawned function itself, and any function sent through a channel.

Everything that crosses in one spawn (the function, its arguments and the
globals) is copied through one sharing map, so an object reached twice arrives
as one copy: `Concurrency::Spawn((c, c), F)` gives the worker one cell, not two.

Cycle detection is handled via a `PointerMap` that tracks already-transferred
traceables.

## Concurrency Functions

Workers and channels are created with functions from `MarmotPrelude/Concurrency.mmt`.
They are called like any other function, but the compiler provides them, so
importing that module is what makes them available:

```marmot-test name=multicore/concurrency_surface path=.doc_example_multicore_surface.mmt module=MulticoreConcurrencySurface
import { "./MarmotPrelude/Concurrency.mmt", "./MarmotPrelude/Prelude/Result.mmt" }

def ComputeRow = fn(row: Int, width: Int) -> Int => row * width;

def w = (42, 800) |> Concurrency::Spawn(ComputeRow);     // w : Worker<Int>
def result = Concurrency::Join(w);                       // result : Result<Int, WorkerError>

def ch : Channel<Int> = Concurrency::MakeChannel(10);    // ch : Channel<Int>
ch -> 42;                                                // send: Bool (false if closed)
def val = <- ch;                                         // receive: Int (blocks if empty)
Concurrency::Close(ch);
```

- `Concurrency::Spawn(argument, F)` takes `F` as an ordinary value: a name, a
  lambda, or a parameter holding one. The argument comes first so a pipe can
  supply it. It stands for all of `F`'s parameters: the value itself
  when `F` takes one, a tuple spread across them when it takes several, and `()`
  when it takes none.
- `Concurrency::Join(w)` blocks and evaluates to `Result<T, WorkerError>`:
  `Ok(value)` when the worker returned, `Err(WorkerError::Cancelled())` when it was
  cancelled, and `Err(WorkerError::Failed(message))` when it stopped with a runtime
  error. A file that joins must also import `MarmotPrelude/Prelude/Result.mmt`.
  `Concurrency.mmt` declares `WorkerError`, and `JoinedOrPanic` for code that
  treats a worker failure as fatal.
- `Concurrency::MakeChannel(capacity)` creates a typed bounded channel. It takes its
  element type from context, so annotate the binding: `def ch : Channel<Int> = ...`.
- `->` (send) and `<-` (receive) are type-checked binary/unary operators

Auxiliary operations: `Concurrency::Close(ch)`, `Concurrency::IsDone(w)`,
`Concurrency::Cancel(w)`. Like the three above, they are compiler-provided and
need `Concurrency.mmt` imported.

`Concurrency::ParallelMap(values, work, chunk_size)` maps `work` over `values` one
worker per chunk. It is ordinary Marmot in `MarmotPrelude/Concurrency.mmt`, not a
compiler builtin: the per-chunk worker is a lambda that captured `work` and crosses
with it.

## Worker Lifecycle

1. **Spawn**: `Concurrency::Spawn(args, Proc)` creates a new `Worker` which:
   - Transfers the spawned function itself, so the worker starts in that function's
     own environment and a captured value is available inside it
   - Creates a `std::jthread`
   - Constructs a new `VirtualMachine` sharing the executable's bytecode
   - Takes a snapshot of dynamic FFI functions from `SharedLibraryCache`
   - Validates FFI thread safety before execution
   - Installs a copy of the spawning VM's globals, serialized on the spawning
     thread at the spawn. Globals are defined once and never reassigned, so
     the spawned function sees exactly what it would see if called directly.
     Module initializers are not re-run, so their effects happen once per
     program, not once per worker.
   - Deep-copies arguments via `ValueTransfer` into the worker VM's stack
   - Calls `VirtualMachine::Execute()` — the same dispatch loop the main VM uses

2. **Join**: `Concurrency::Join(w)` blocks until the worker finishes. The worker's value is
   deserialized from its `SerializedValue` and wrapped in `Ok`; a failure or
   cancellation becomes `Err(WorkerError)` instead. `JOIN_WORKER` builds these
   union values itself, using the constructor tags the compiler reads from the
   `Result` and `WorkerError` declarations.

3. **Cancel**: `Concurrency::Cancel(w)` requests cooperative cancellation via
   `std::jthread::request_stop()`. The worker observes the request at
   safepoints: loop back-edges (`JUMP_BACK`), tail calls (`TAIL_CALL`), and the
   return of any foreign call. A worker blocked in `ch -> v` or `<- ch` is woken
   immediately, because channel waits take the stop token, and the builtin
   `Sleep` is interruptible for the same reason. On observing cancellation the
   worker terminates with the `WorkerCancelled` runtime error.

   Cancellation is cooperative, so it is bounded by whatever the worker is
   currently doing. Blocking calls that do not consult the stop token — stdin
   reads and third-party dynamic FFI — still run to completion first. See
   `common/src/Common/Cancellation/Cancellation.h`.

4. **Poll**: `Concurrency::IsDone(w)` checks if the worker has completed without blocking.

## Failure Propagation

- If a worker fails, the `Worker` captures both the error message and the
  originating `RuntimeErrorCode`
- On a join, that failure becomes a value, not an error in the joining VM: a
  `WorkerCancelled` code gives `Err(WorkerError::Cancelled())`, and any other
  code gives `Err(WorkerError::Failed(message))`. The joining program keeps
  running and decides what a failed worker means
- `Panic::Panic` inside a worker fails that worker and nothing else. Panic exits
  the process, and a worker may not do that to a program it only borrows a thread
  from, so the runtime stops the worker with `WorkerExited` and the joiner gets
  `Err(WorkerError::Failed(...))`. The panic message still goes to standard error
- If a worker is never joined and failed, the destructor prints the message to
  standard output

## FFI Under Multicore

Dynamic FFI libraries declare thread safety in `package.marmot`:

```toml
[ffi]
enabled = true
abi_version = 1
thread_safe = true
```

- Default: `thread_safe = false` (safe by default)
- Validation happens at worker construction time, before `ExecuteLoop()`
- If any loaded package has `thread_safe = false`, worker creation fails with
  a clear error naming the offending packages
- `thread_safe = true` means the native code uses no global mutable state, or
  manages its own synchronization

## Channels

Channels provide typed message passing between workers:

- `Concurrency::MakeChannel(capacity)` — creates a bounded `Channel<T>`, with `T` taken from context
- `ch -> value` — sends a value (blocks if full, returns `Bool`)
- `<- ch` — receives a value (blocks if empty, runtime error if closed and empty)
- `Concurrency::Close(ch)` — closes the channel, unblocks all waiters

There is no bounded or non-blocking receive at the language level: a receive
waits until a value arrives, the channel closes, or the worker is cancelled.
`Channel::TryReceive` exists in the runtime but has no opcode or syntax, so it
is currently unreachable from Marmot code.

Internally: `std::mutex` + `std::condition_variable_any` +
`std::deque<SerializedValue>`. The waits are stop-token-aware, which is what
makes a channel-blocked worker cancellable.

Channel handles are raw `Int` values managed by `ChannelRegistry`, not
VM-heap objects. `Channel<T>` is itself `Transferable`, so channels can be
passed directly to spawned workers. A channel is reclaimed from the registry
once it is both closed and drained; operations on a reclaimed handle behave
like operations on a closed channel.

## Relationship to OS Threads

1:1 mapping: each worker maps to one OS thread via `std::jthread`. Workers are
meant to be few and long-lived, not lightweight tasks. Spawning thousands of
workers will exhaust OS thread limits.

## Single-Threaded Overhead

Nearly all of the concurrency system — `Worker`, `Channel`, the registries,
`ValueTransfer`, FFI thread-safety validation — lives outside
`VirtualMachine::ExecuteLoop()` and costs the main VM nothing.

The exception is cancellation. Making a running worker stoppable requires
safepoints *inside* the dispatch loop, so `ExecuteLoop()` now checks
`IsCancellationRequested()` at loop back-edges, at tail calls, and after
foreign calls return. The check is `m_stop_possible && m_stop_token.stop_requested()`,
and `m_stop_possible` is false for the main VM because only `Worker` ever calls
`SetStopToken`. The main VM therefore pays one never-taken, correctly predicted
branch at those sites and never touches the atomic. GC triggers, allocator
calls, stack operations, and FFI dispatch remain identical for main and worker
VMs.

The `DynamicFFIRegistry` refactor replaces a Meyer's singleton `GetInstance()`
(hidden branch + pointer load on every call) with a direct member lookup on the
VM instance (equal or cheaper).

## Future Extensibility

None of the following is scheduled work.

- Bounded and multi-channel waiting (`try_receive`, `select`, timeouts)
- M:N scheduling (green threads mapped to a thread pool)
- Work stealing for automatic load balancing
- Shared-memory opt-in for performance-critical workloads
- `Future<T>` / `Promise<T>` as single-value channels
- `ParallelMap`, `ParallelFor`, `ParallelReduce` collection helpers
