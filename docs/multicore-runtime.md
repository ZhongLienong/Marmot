# Marmot Multicore Runtime

## Execution Model: Isolated Workers

Marmot uses an **isolated-worker** concurrency model. Each worker is a full
`VirtualMachine` instance with its own allocator, garbage collector, value
stack, call stack, and global variables. Workers share read-only bytecode
(`std::shared_ptr<const VmExecutable>`) but cannot share mutable values.

Workers exchange typed values through spawn arguments, join results, and
channel messages. The VM copies those values across heaps.

## What Is Shared vs Isolated

| Component | Shared? | Notes |
|-----------|---------|-------|
| Bytecode (`VmExecutable`) | Yes (read-only) | Via `shared_ptr<const>` |
| VM intrinsic table | Yes (immutable) | `constexpr` array, compile-time |
| Dynamic FFI function pointers | Snapshot per VM | Lock-free lookup during execution |
| Shared library handles | Yes (cache) | `SharedLibraryCache` singleton, load-time only |
| Value stack | No | Per VM, 1,048,576 value slots |
| Call stack | No | Per VM, 524,288 call frames |
| Heap / Allocator | No | Per-VM `MarmotAllocator` |
| Garbage collector | No | Per-VM, independent generational collection |
| Global variables | No | Per-VM; a worker holds copies of the globals its code can read |
| String literal cache | No | Per-VM |
| Static closure cache | No | Per-VM |

## Value Transfer and the `Transferable` Typeclass

Cross-worker value transfer is governed by the `Transferable<T>` typeclass.
Types that satisfy `Transferable` can be deep-copied across worker boundaries
by `ValueTransfer`: the sending VM copies the value into a `TransferPayload`,
and the receiving VM rebuilds it in its own heap. Spawns, joins and channels
all cross this way.

**Built-in `Transferable` instances** (deep copy):
- `Int`, `Float`, `Byte`, `Word`, `Bool`, `Unit` — bitwise copy (no heap allocation)
- `Text` — new allocation in target VM, bytes copied
- `Array<T>` where `Transferable<T>` — new allocation, elements recursively transferred
- `Channel<T>` where `Transferable<T>` — handle copy (index in the channel registry)

**Derivable for user types**:
- Structs and unions can `deriving (Transferable)` if all fields/variants satisfy `Transferable`
- Tuples are transferable if all element types satisfy `Transferable`

**Never transferable** (no `Transferable` instance):
- `Ref<T>` — a reference belongs to the VM that allocated it
- Ranges — semantically non-portable
- `Worker<T>` — its handle belongs to the VM that spawned it

**Functions** cross as a procedure index and a copy of their captured values.
The compiler infers transferability when every capture is transferable,
including for functions with no captures. A `where Transferable<fn(A) -> B>`
constraint requires transferable captures for callback parameters of that
signature. A `Channel<fn(A) -> B>` accepts only transferable functions.

Transferability is checked at compile time. Attempting to spawn with a
non-transferable argument or create a `Channel<T>` where `T` lacks a
`Transferable` instance produces a constraint-failure error.

The VM also checks the values it serializes. A reference reached through a
global or supplied by older bytecode causes `InvalidWorkerTransfer` during
`Spawn` or `Send`. If a worker reaches a reference while serializing its result,
`Join` reports `Err(WorkerError::Failed(...))`.

Everything that crosses at once (a spawn's function and arguments, a message, a
join's result) is one payload, a table of the objects it reaches in which each
object is copied once and refers to the others by index. An object reached twice
arrives as one copy, and a cycle is an index pointing back. Copying runs from a worklist in
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
worker takes a global the first time a payload brings it and keeps that copy.
Using a worker handle reached through a global from a different VM raises
`InvalidWorkerHandle`.

## Concurrency Functions

Workers and channels are created with functions from `MarmotPrelude/Concurrency.mmt`.
Most are primitives, each one instruction of the VM, which the module declares as a
`foreign` with no library and exports like anything else, so `use Concurrency.{ Spawn }`
works. A primitive is called like a function, but it is not a value:

```marmot-test name=multicore/concurrency_surface path=.doc_example_multicore_surface.mmt module=MulticoreConcurrencySurface
import { "./projects/marmotstdlib/MarmotPrelude/Concurrency.mmt", "./projects/marmotstdlib/MarmotPrelude/Prelude/Result.mmt" }

def ComputeRow = fn(row: Int, width: Int) -> Int => row * width;

def w = (42, 800) |> Concurrency::Spawn(ComputeRow);     // w : Worker<Int>
def result = Concurrency::Join(w);                       // result : Result<Int, WorkerError>

def ch : Channel<Int> = Concurrency::MakeChannel(10);    // ch : Channel<Int>
Concurrency::Send(ch, 42);                              // Bool
def val = Concurrency::Receive(ch);                     // Option<Int>
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
- `Concurrency::Send(ch, value)` is a primitive. `Concurrency::Receive(ch)` wraps a private primitive and returns `Option<T>`.

Auxiliary operations: `Concurrency::Close(ch)`, `Concurrency::IsDone(w)`,
`Concurrency::Cancel(w)`. Like `Spawn`, they are primitives declared in
`Concurrency.mmt`.

`Concurrency::ParallelMap(values, work, chunk_size)` maps `work` over `values` one
worker per chunk. It is ordinary Marmot in `MarmotPrelude/Concurrency.mmt`, not a
primitive: the per-chunk worker is a lambda that captured `work` and crosses
with it.

## Worker Lifecycle

1. **Spawn** copies the function, its captures, its arguments, and the globals
   it can read into a payload. Native-thread mode starts a dedicated
   `std::jthread`. Cooperative mode queues the worker on its assigned thread.
   On its first run, the worker validates FFI thread safety where required,
   creates its VM, and installs the payload. The VM shares executable bytecode
   and takes a snapshot of dynamic FFI functions. Module initializers run once
   per program.

2. **Join** waits for the worker to finish. In native-thread mode the caller's
   OS thread waits. In cooperative mode the scheduler parks the calling VM.
   `JOIN_WORKER` deserializes the result into the calling VM and builds the
   private outcome that `Concurrency::Join` converts to `Result<T, WorkerError>`.

3. **Cancel** requests cancellation through the worker's
   stop source. Running code checks for cancellation at loop back-edges, tail
   calls, returns, and after foreign calls return. In native-thread mode,
   channel waits and `Sleep` observe the stop token. In cooperative mode the
   scheduler wakes a parked worker to observe cancellation, except when
   `RunProcess` has already started. Blocking stdin reads and synchronous
   third-party native calls run to completion before the worker can observe
   cancellation.

4. **Poll** checks whether the worker has completed. `Concurrency::IsDone(w)`
   returns immediately.

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

Third-party FFI libraries declare thread safety for native-thread workers and
cooperative runs with multiple scheduler threads in `package.marmot`:

```toml
[ffi]
enabled = true
thread_safe = true
```

- The flag defaults to `false`. When validation applies, a worker fails on its
  first run if any third-party library imported by its program has
  `thread_safe = false`.
- Single-thread cooperative runs can interleave calls from different VMs on the
  same OS thread. See [FFI ABI](ffi-abi.md) for native state and call behavior.

## Channels

Channels provide typed message passing between workers:

- `Concurrency::MakeChannel(capacity)` — creates a bounded `Channel<T>`, with `T` taken from context.
  Any positive `Int` works as the capacity. Values below 1 become 1. A send
  can proceed as soon as the buffer has space.
- `Concurrency::Send(ch, value)` waits for space, sends the value, and returns `true`. A closed channel returns `false`.
- `Concurrency::Receive(ch)` waits for a message and returns `Some(value)`. Once the channel is closed and drained, it returns `None()`.
- `Concurrency::Close(ch)` — closes the channel, unblocks all waiters

There is no bounded or non-blocking receive at the language level: a receive
waits until a value arrives, the channel closes, or the worker is cancelled.

Native-thread mode uses stop-token-aware condition-variable waits. In
cooperative mode the scheduler parks any VM whose send or receive would block
and makes it runnable when the channel can make progress or closes.

Channel handles are raw `Int` values managed by `ChannelRegistry`, not
VM-heap objects. `Channel<T>` is itself `Transferable`, so channels can be
passed directly to spawned workers. The cooperative scheduler owns the registry
for its run. The registry reclaims a channel once it is closed and drained.
Operations on a reclaimed handle behave like operations on a closed channel.

## Relationship to OS Threads

By default, each spawned worker runs on its own OS thread via `std::jthread`.
`marmotvm run <program.mmc> --cooperative-workers` enables green threads, with
one scheduler thread by default. `--worker-threads N` sets the total, including
the calling thread, which runs the main VM. The scheduler assigns each spawned
worker to a thread in round-robin order and keeps it there.

The scheduler parks VMs on joins, channel waits, and positive sleeps. It
requeues a running VM after 1,024 scheduling checkpoints, reached at calls,
tail calls, returns, and loop back-edges. `System::RunProcess` parks the calling
VM while a pool of up to four helper threads launches and waits for child
processes. A queued request can be cancelled before launch. A running request
finishes before its worker observes cancellation. Other native calls execute
synchronously on the scheduler thread and can block other VMs assigned to it.

When the main VM returns or calls `System::Exit`, the scheduler stops dispatching
workers and waits for any native calls already running.
If all VMs are parked and no timer or native process completion can wake one,
the scheduler reports `ConcurrencyDeadlock`.

## Dispatch and Cancellation

Both dispatch specializations retain cancellation checks. The main VM's stop
token has no stop state, so those checks require no atomic read. The
cooperative specialization also counts scheduling checkpoints. Each VM
continues to manage its own heap, garbage collector, and stacks.

## Future Extensibility

None of the following is scheduled work.

- Bounded and multi-channel waiting (`try_receive`, `select`, timeouts)
- Work stealing for automatic load balancing
- Shared-memory opt-in for performance-critical workloads
- `Future<T>` / `Promise<T>` as single-value channels
- `ParallelFor` and `ParallelReduce` collection helpers beside `ParallelMap`
