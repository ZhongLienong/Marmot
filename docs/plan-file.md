# Build Plans

A build plan is a JSON file that lists everything a compile needs. With a
plan, the compiler looks nothing up for itself: it reads no `project.marmot`,
`package.marmot` or `marmot.lock`, and ignores `MARMOT_PATH`.

```powershell
marmot run --plan build/plan.json
marmot check --plan build/plan.json --format json
marmot build --plan build/plan.json
```

The plan replaces the source-file argument; passing both is an error. `build`
writes the artifact beside the plan's entry, as it would for that file.

Plans are meant to be written by tools: something that resolves a project and
its dependencies writes the plan, and the compiler only compiles. Without a
plan, the CLI resolves inputs itself (see [Import Resolution](module-system.md#import-resolution)).

## Format

```json
{
  "version": 1,
  "entry": "src/Main.mmt",
  "search_paths": ["src", "packages/Image", "MarmotPrelude"],
  "native_packages": [
    {
      "name": "Image",
      "root": "packages/Image",
      "library": "packages/Image/lib/windows/x64/marmot_image.dll",
      "functions": {
        "MIDORI_FFI_Image_ReadInfo": "marmot_image_read_info"
      },
      "thread_safe": true,
      "checksum": "sha256:..."
    }
  ]
}
```

| Member | Required | Meaning |
|---|---|---|
| `version` | yes | Plan format version. This compiler reads `1`. |
| `entry` | yes | The file to compile. |
| `search_paths` | no | Directories searched, in order, for `<Name>` imports. Each must exist. |
| `native_packages` | no | Packages that ship a native library (below). |

Each native package:

| Member | Required | Meaning |
|---|---|---|
| `name` | yes | Package name; unique within the plan. |
| `root` | yes | The package's directory. Source files directly in it may declare the package's foreign functions. |
| `library` | yes | The shared library. If the file is missing, the program still compiles and runs, and calls into it fail. |
| `functions` | yes | Foreign name used in Marmot mapped to the symbol exported by the library. |
| `thread_safe` | no | Whether workers may call the library concurrently. Default `false`. |
| `checksum` | no | Expected checksum of the library, verified before it loads. |

Relative paths are relative to the directory containing the plan file.

A plan is checked before anything compiles. Unknown members, wrong types, a
missing entry and search paths that do not exist are all errors that name the
offending member, for example `search_paths[1]: not a directory: ...` or
`plan: unknown member "serach_paths"`. A misspelt input is reported rather
than silently left out.

## Native libraries

Checking or building a program never loads its native libraries; loading one
runs the library's own code. `marmot run` loads the libraries of every native
package the program's files belong to just before the program starts, and a
library that fails to load stops the run with an error.
