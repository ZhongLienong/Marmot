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
was supplied, then checks for the required exports. A native call runs on
the calling VM's thread. A native fault reaches the VM's platform fault
handler. The ABI provides no per-call recovery contract.

Marmot workers require every library imported by the program to declare
`thread_safe = true`. That contract covers callable exports and result
cleanup. The toolchain supplies this policy for the reference library.
Its file operations, process launches and local time operations coordinate
with changes to the process working directory and environment. Those changes
are visible to all workers. Each worker retains its own last I/O and system
errors and random number generator.
