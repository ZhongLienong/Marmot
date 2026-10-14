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
| Global variables | No | Per-VM; a worker holds copies of the globals its code can read |
| String literal cache | No | Per-VM |
| Static closure cache | No | Per-VM |

## Value Transfer and the `Transferable` Typeclass

Cross-worker value transfer is governed by the `Transferable<T>` typeclass.
Types that satisfy `Transferable` can be deep-copied across worker boundaries
by `ValueTransfer`: the sending thread copies the value into a `TransferPayload`,
and the receiving VM rebuilds it in its own heap. Spawns, joins and channels
all cross this way.

**Built-in `Transferable` instances** (deep copy):
- `Int`, `Float`, `Byte`, `Word`, `Bool`, `Unit` — bitwise copy (no heap allocation)
- `Text` — new allocation in target VM, bytes copied
- `Array<T>` where `Transferable<T>` — new allocation, elements recursively transferred
- `Channel<T>` where `Transferable<T>` — handle copy (process-wide registry index)
- `Ref<T>` where `Transferable<T>` — **copied**: the worker gets its own cell, so
  writes on either side are not seen by the other. The compiler warns
  (`CellCrossesWorker`) wherever a cell is passed to `Spawn`, sent through a
  channel, or passed to a function that hands that parameter to a worker, as
  `ParallelMap` does its values; wherever a function that crosses captures one
  or reads one as a global, itself or through a function of the same module it
  calls; and wherever one comes back from a worker, through `Join` or a function
  built on it. Which parameters a function hands across is found from its body,
  across modules.

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
included, are immutable once built. The one exception is a `Ref<T>`, whose
contents can change; it is copied like everything else, which is why crossing
with one is a warning rather than a data race. The same copy carries a worker's
globals, the spawned function itself, and any function sent through a channel.

Everything that crosses at once (a spawn's function and arguments, a message, a
join's result) is one payload, a table of the objects it reaches in which each
object is copied once and refers to the others by index. An object reached twice
arrives as one copy, `Concurrency::Spawn((c, c), F)` gives the worker one cell,
not two, and a cycle is an index pointing back. Copying runs from a worklist in
both directions, so a structure of any depth, such as a long linked list, takes
no native stack. Each value crosses as the kind the compiler gave it, a
value's by its type, an array's elements by the array's flag and an
aggregate's fields by its shape, never as its bits suggest.

A payload also carries the globals its functions can read. The sending VM
follows each closure in it to its procedure, whose bytecode names the globals
it reads and the procedures it calls or makes closures of, and follows those in
turn, along with the closures inside each global it copies. A worker therefore
starts with the globals its own code can read and no others, and a function
that reaches it later, through a channel or a join, brings the ones it reads. A
worker takes a global the first time a payload brings it and keeps that copy,
so a cell held in a global is one cell for the whole worker; the VM that ran
the program's initializers already holds every global and takes none.

## Concurrency Functions

Workers and channels are created with functions from `MarmotPrelude/Concurrency.mmt`.
Most are primitives, each one instruction of the VM, which the module declares as a
`foreign` with no library and exports like anything else, so `use Concurrency.{ Spawn }`
works. A primitive is called like a function, but it is not a value:

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
  error. Matching on it, as `Result::Ok(value)`, needs `MarmotPrelude/Prelude/Result.mmt`
  imported. `Concurrency.mmt` declares `WorkerError`, and `JoinedOrPanic` for code that
  treats a worker failure as fatal.
- `Concurrency::MakeChannel(capacity)` creates a typed bounded channel. It takes its
  element type from context, so annotate the binding: `def ch : Channel<Int> = ...`.
  It is ordinary Marmot, `where Transferable<T>`, over a primitive the module keeps
  to itself.
- `->` (send) and `<-` (receive) are type-checked binary/unary operators

Auxiliary operations: `Concurrency::Close(ch)`, `Concurrency::IsDone(w)`,
`Concurrency::Cancel(w)`. Like `Spawn` and `Join`, they are primitives declared in
`Concurrency.mmt`.

`Concurrency::ParallelMap(values, work, chunk_size)` maps `work` over `values` one
worker per chunk. It is ordinary Marmot in `MarmotPrelude/Concurrency.mmt`, not a
primitive: the per-chunk worker is a lambda that captured `work` and crosses
with it.

## Worker Lifecycle

1. **Spawn**: `Concurrency::Spawn(args, Proc)` creates a new `Worker` which:
   - Transfers the spawned function itself, so the worker starts in that function's
     own environment and a captured value is available inside it
   - Creates a `std::jthread`
   - Constructs a new `VirtualMachine` sharing the executable's bytecode
   - Takes a snapshot of dynamic FFI functions from `SharedLibraryCache`
   - Validates FFI thread safety before execution
   - Installs the globals its code can read, copied on the spawning thread at
     the spawn. Globals are defined once and never reassigned, so the spawned
     function sees exactly what it would see if called directly. Module
     initializers are not re-run, so their effects happen once per program,
     not once per worker.
   - Deep-copies arguments via `ValueTransfer` into the worker VM's stack
   - Calls `VirtualMachine::Execute()` — the same dispatch loop the main VM uses

2. **Join**: `Concurrency::Join(w)` blocks until the worker finishes. The worker's value is
   deserialized from its `TransferPayload` and wrapped in `Ok`; a failure or
   cancellation becomes `Err(WorkerError)` instead. `JOIN_WORKER` builds these
   union values itself, using the constructor tags the compiler reads from the
   `Result` and `WorkerError` declarations.

3. **Cancel**: `Concurrency::Cancel(w)` requests cooperative cancellation via
   `std::jthread::request_stop()`. The worker observes the request at
   safepoints: loop back-edges (`JUMP_BACK`), tail calls (`TAIL_CALL`), returns
   (`RETURN`), and the return of any foreign call. A worker blocked in `ch -> v` or `<- ch` is woken
   immediately, because channel waits take the stop token, and the builtin
   `Sleep` is interruptible for the same reason. On observing cancellation the
   worker terminates with the `WorkerCancelled` runtime error.

   Cancellation is cooperative, so it is bounded by whatever the worker is
   currently doing. Blocking calls that do not consult the stop token — stdin
   reads and third-party dynamic FFI — still run to completion first. See
   `projects/marmotvm/src/Support/Cancellation/Cancellation.h`.

4. **Poll**: `Concurrency::IsDone(w)` checks if the worker has completed without blocking.

## Failure Propagation

- If a worker fails, the `Worker` keeps the error message and whether the
  failure was its cancellation
- On a join, that failure becomes a value, not an error in the joining VM: a
  cancellation gives `Err(WorkerError::Cancelled())`, and any other failure,
  a native library that is not thread-safe included, gives
  `Err(WorkerError::Failed(message))`. The joining program keeps running and
  decides what a failed worker means
- `Panic::Panic` inside a worker fails that worker and nothing else. Panic exits
  the process, and a worker may not do that to a program it only borrows a thread
  from, so the runtime stops the worker with `WorkerExited` and the joiner gets
  `Err(WorkerError::Failed(...))`. The panic message still goes to standard error
- If a worker is never joined and failed, the destructor prints the message to
  standard error. A worker still running when the program ends is stopped, and
  that cancellation is not reported

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
- If any native library the program imports has `thread_safe = false`,
  worker creation fails with a clear error naming the offending libraries.
  Libraries loaded for another program in the same process do not count
- `thread_safe = true` means the native code uses no global mutable state, or
  manages its own synchronization

## Channels

Channels provide typed message passing between workers:

- `Concurrency::MakeChannel(capacity)` — creates a bounded `Channel<T>`, with `T` taken from context.
  A capacity below 1 is taken as 1: there is no unbuffered channel, so a send
  never waits for a receiver
- `ch -> value` — sends a value (blocks if full, returns `Bool`)
- `<- ch` — receives a value (blocks if empty, a `ChannelClosed` error if closed and empty)
- `Concurrency::Close(ch)` — closes the channel, unblocks all waiters

There is no bounded or non-blocking receive at the language level: a receive
waits until a value arrives, the channel closes, or the worker is cancelled.

Internally: `std::mutex` + `std::condition_variable_any` +
`std::deque<TransferPayload>`. The waits are stop-token-aware, which is what
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
`IsCancellationRequested()` at loop back-edges, at tail calls, at returns, and
after foreign calls return. The check is `m_stop_token.stop_requested()`, which
reads the token's atomic only when the token has a stop state, and the main
VM's token never has one because only `Worker` ever calls `SetStopToken`. The
main VM therefore pays one never-taken, correctly predicted branch at those
sites and never touches the atomic. GC triggers, allocator
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
- `ParallelFor` and `ParallelReduce` collection helpers beside `ParallelMap`
