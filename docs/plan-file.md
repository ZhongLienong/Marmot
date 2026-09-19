# Build Plans

A build plan is a JSON file that lists everything a compile needs. With a
plan, the compiler looks nothing up for itself: it reads no `project.marmot`,
`package.marmot` or `marmot.lock`, and ignores `MARMOT_PATH`.

```powershell
marmotc run --plan build/plan.json
marmotc check --plan build/plan.json --format json
marmotc build --plan build/plan.json
marmotc test --plan build/plan.json --dir test
```

The plan replaces the source-file argument; passing both is an error. `build`
writes the artifact beside the plan's entry, as it would for that file. `test`
compiles every test file with the plan's inputs, so its plan has no entry.

The `marmot` tool writes the plan: it resolves a project and its dependencies,
and the compiler only compiles. `marmot plan [file]` prints the plan it would
use. Without a plan, `marmotc` finds `<Name>` imports through `MARMOT_PATH` and
native libraries through `--library-path`, `MARMOT_LIBRARY_PATH` and the
declaring module's directory (see [Import Resolution](module-system.md#import-resolution)
and [Native libraries](#native-libraries)).

## Format

```json
{
  "version": 1,
  "entry": "src/Main.mmt",
  "search_paths": ["src", "packages/Image", "MarmotPrelude"],
  "library_paths": ["native/bin"],
  "native_libraries": [
    {
      "name": "marmot_image",
      "path": "packages/Image/lib/windows/x64/marmot_image.dll",
      "thread_safe": true,
      "checksum": "sha256:..."
    }
  ]
}
```

| Member | Required | Meaning |
|---|---|---|
| `version` | yes | Plan format version. This compiler reads `1`. |
| `entry` | for run, check, build | The file to compile. Absent in a plan for `test`. |
| `search_paths` | no | Directories searched, in order, for `<Name>` imports. Each must exist. |
| `library_paths` | no | Directories searched for native libraries. Each must exist. |
| `native_libraries` | no | Settings for libraries that source names with `from "name"` (below). |

Each native library:

| Member | Required | Meaning |
|---|---|---|
| `name` | yes | The name source gives the library in `foreign ... from "name"`; unique within the plan. |
| `path` | no | The library file, tried before any search. |
| `thread_safe` | no | Whether workers may call the library concurrently. Default `false`. |
| `checksum` | no | Expected checksum of the library, verified before it loads. |

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
runs the library's own code. `marmotc run` loads them just before the program
starts. For a library named `name` it looks for `name.dll` on Windows,
`libname.dylib` on macOS and `libname.so` on Linux, trying in order:

1. the plan's `path` for the library
2. `--library-path` directories, then the plan's `library_paths`, then
   `MARMOT_LIBRARY_PATH`
3. the declaring module's `lib/<platform>/` directory, then the module's own
   directory, where `<platform>` is `windows/x64`, `macos` or `linux/x86_64`

A library that is not found, fails to load, or lacks a symbol the program uses
stops the run with an error before the program starts.
