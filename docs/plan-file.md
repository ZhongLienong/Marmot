# Build Plans

A build plan is a JSON file that lists everything a compile needs. With a
plan, the compiler looks nothing up for itself: it reads no `project.marmot`,
`package.marmot` or `marmot.lock`, and ignores `MARMOT_PATH`.

```powershell
marmotc check --plan build/plan.json --format json
marmotc build --plan build/plan.json -o target/Main.mmc
marmotc build --plan build/inputs.json test/smoke.mmt -o target/test/smoke.mmc
```

A plan with an entry replaces the source-file argument; passing both is an
error. A plan without an entry gives only the inputs, and the source file given
with it is the entry: `marmot test` builds each test that way. `build` writes
the `.mmc` beside the entry, or to `-o`; `marmotvm` runs it.

The `marmot` tool writes the plan: it resolves a project and its dependencies,
and the compiler only compiles. `marmot plan [file]` prints the plan it would
use. Without a plan, `marmotc` finds `<Name>` imports through `MARMOT_PATH`
(see [Import Resolution](module-system.md#import-resolution)).

## Format

```json
{
  "version": 1,
  "entry": "src/Main.mmt",
  "search_paths": ["src", "packages/Image", "MarmotPrelude"],
  "native_libraries": [
    { "name": "marmot_image", "thread_safe": true, "checksum": "sha256:..." }
  ]
}
```

| Member | Required | Meaning |
|---|---|---|
| `version` | yes | Plan format version. This compiler reads `1`. |
| `entry` | for run, check, build | The file to compile. Absent in a plan for `test`. |
| `search_paths` | no | Directories searched, in order, for `<Name>` imports. Each must exist. |
| `native_libraries` | no | How the libraries that source names with `from "name"` may be used (below). |

Each native library:

| Member | Required | Meaning |
|---|---|---|
| `name` | yes | The name source gives the library in `foreign ... from "name"`; unique within the plan. |
| `thread_safe` | no | Whether workers may call the library concurrently. Default `false`. |
| `checksum` | no | Expected checksum of the library, verified before it loads. |

The compiler records `thread_safe` and `checksum` in the program. Where a
library's file is belongs to the machine that runs the program, so a plan has no
library paths: the run is given them (see below).

Relative paths are relative to the directory containing the plan file.

A plan is checked before anything compiles. Unknown members, wrong types, a
missing entry and search paths that do not exist are all errors that name the
offending member, for example `search_paths[1]: not a directory: ...` or
`plan: unknown member "serach_paths"`. A misspelt input is reported rather
than silently left out.

## Native libraries

Source names the library a foreign function comes from:

```marmot
foreign "marmot_image_read_info" ReadInfo : fn(Text) -> Int from "marmot_image";

foreign "marmot_image"
{
    "marmot_image_width" Width : fn(Int) -> Int;
    "marmot_image_height" Height : fn(Int) -> Int;
}
```

The quoted name before the Marmot name is the symbol the library exports. The
compiler records each library, the symbols used from it and the directory of
the module that declared it, in the program and in its `.mmc`.

Checking or building a program never loads its native libraries; loading one
runs the library's own code. `marmotvm` loads them just before the program
starts. For a library named `name` it looks for `name.dll` on Windows,
`libname.dylib` on macOS and `libname.so` on Linux, trying in order:

1. the file given with `--library name=<file>`
2. `--library-path` directories, then `MARMOT_LIBRARY_PATH`
3. the declaring module's `lib/<platform>/` directory, then the module's own
   directory, where `<platform>` is `windows/x64`, `macos` or `linux/x86_64`

`marmot run` passes `--library` for the library of every package it resolved.
A library that is not found, fails to load, or lacks a symbol the program uses
stops the run with an error before the program starts.
