# Current standalone bundle format (under development)

This document describes the bundle layout that the current packager writes and the current VM reads. The layout and its identifiers can change as Marmot develops.

On Windows and Linux, `marmot build --executable` appends a package to a native `marmotvm` executable. The package contains one [`.mmc` artifact](../mmc/format.md) and the native libraries it imports.

## VM command line

`marmotvm --standalone-version` exits successfully and writes a single JSON object to stdout. The packager currently checks that the `version` field equals `1` before it uses the reported `executable` as the bundle prefix. `executable` is the absolute path to the running VM binary as a UTF-8 JSON string. If the VM cannot find its executable, it reports the error on stderr and exits unsuccessfully.

`marmotvm run <program.mmc> --library <name>=<path>` binds a logical native library name from the program's imports to a file. Repeat `--library` for multiple names. Both parts must be nonempty. The last mapping for a name wins. An explicit path is the only filesystem candidate for its name, and a relative path is relative to the working directory.

Without a mapping, the VM checks `--library-path` directories in argument order, then `MARMOT_LIBRARY_PATH`, then the VM's directory and the matching local standard library build directory when running from a checkout build. It then checks the platform-specific subdirectory and root of each hint directory in the artifact. It uses the platform library filename for the logical name, such as `example.dll` on Windows or `libexample.so` on Linux.

## Binary layout

All lengths and counts are unsigned 64-bit little-endian integers. A blob is a byte length followed by exactly that many bytes.

| Order | Field | Encoding |
|---:|---|---|
| 1 | VM executable | Complete platform-native executable image |
| 2 | Package | One `.mmc` blob, then a library count and that many library records |
| 3 | Package size | Number of bytes in field 2 |
| 4 | Magic | 16 bytes, ASCII `MARMOT-EXE-v1` followed by three `00` bytes |

The footer is the last 24 bytes of the executable. The loader locates the package by subtracting its size from the start of the footer. Each library record contains three blobs in order. The first is the UTF-8 logical name, the second is the UTF-8 file basename, and the third is the complete library binary. Every library named by the `.mmc` appears exactly once. The file basename must be a single filename other than `.` or `..` and must contain no NUL byte.

On startup, the VM checks the footer of its own executable. When it finds a package, it reads the `.mmc`, extracts each bundled library to a temporary directory, and binds it to the recorded name. The VM runs the embedded program with the arguments passed to the executable. Without the footer magic, the VM processes its regular command line.
