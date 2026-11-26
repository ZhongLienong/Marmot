# FFI ABI

Marmot native libraries follow the [native extension ABI](../format/ffi/ABI.md).
The ABI reference defines the calling convention, argument slots, and result
ownership. The SDK ships the C99 header and a standalone extension example.

The `marmot` tool resolves package libraries and the reference standard
library, `marmotstdlib`, and passes their paths to the VM. To use a different
build of the reference library, set `MARMOTSTDLIB` to its shared-library file.
For direct VM invocations, use `--library name=path`, `--library-path <dir>`
or `MARMOT_LIBRARY_PATH`. The compiler records logical library names in
`.mmc` programs. It can check or build a program before those libraries have
been built.

Before startup, the VM checks a selected prebuilt library's checksum when one
was supplied, then checks for the required exports. Native calls generally run
synchronously on the calling VM's thread, so a blocking call can pause other
VMs assigned to the same scheduler thread. In cooperative mode the standard
library's `System::RunProcess` runs on a bounded helper pool and parks the
calling VM. Faults from synchronous calls reach the VM's platform fault
handler. The ABI provides no per-call recovery contract.

Native-thread workers and cooperative workers using multiple scheduler threads
require the program's third-party native libraries to declare
`thread_safe = true`. That contract covers callable exports and result cleanup.
With one scheduler thread, calls from different VMs can interleave on that
thread, so native thread-local state can be shared across workers.

The reference library's file operations, process launches, and local time
operations coordinate with changes to the process working directory and
environment. Those changes are visible to all workers. Cooperative mode
requires a compatible build when the program imports `marmotstdlib`. The VM
uses that library's hooks to preserve each VM's last I/O and system errors
across slices. The random number generator is OS-thread-local, so workers
assigned to the same scheduler thread use the same generator.
