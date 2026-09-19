# Package System

Marmot has a local package dependency system built around `package.marmot`,
`project.marmot`, `marmot.lock`, and project-local vendoring into `packages/`.
It lives in the `marmot` project tool (`tool/`); the compiler, `marmotc`, reads
no manifests and is given its inputs in a [build plan](plan-file.md).
The current implementation resolves from package directories that already exist
on disk. Remote registry fetch and publish support are still deferred.

## What Exists Today

- semantic version parsing and comparison, including prerelease and build metadata
- dependency constraints in `project.marmot` and `package.marmot`
- compiler version validation through `package.marmot` `marmot_version`
- local package index scanning across project, cache, and path roots
- dependency resolution with cycle and version-conflict diagnostics
- project-local vendoring into `packages/<name>-<version>/`
- `marmot.lock` generation and reuse
- package source checksum recording in the lockfile
- checksum verification for selected prebuilt native libraries
- FFI ABI version validation through `package.marmot` `[ffi].abi_version`
- CLI package commands: `marmot install`, `marmot update`, `marmot remove`, `marmot list`
- import-triggered dynamic FFI loading for resolved packages

## Still Deferred

- remote registry fetch and publish workflows
- automatic native builds from `native/` or `[build]`
- manifest-driven export enforcement beyond normal module `public export`
- targeted `marmot update <package>` resolution; the current command refreshes the whole graph
- full garbage collection of every newly orphaned transitive vendored package directory

Because registry fetching is not implemented yet, fully reproducible installs on
a clean machine currently depend on the vendored `packages/` directory or some
other local package root already being available.

## Package Discovery

Resolution is based on a local package index. Marmot scans these roots, in this
order:

1. the active project's `packages_dir` or `packages/`
2. the global cache directory
3. `project.marmot` `marmot_path` entries
4. existing `MARMOT_PATH` entries

Each root contributes:

- the root itself, if it contains `package.marmot`
- any immediate child directories that contain `package.marmot`

Available versions are sorted by:

1. highest semantic version
2. root priority for ties
3. path for deterministic ties

That means a newer compatible version in a lower-priority root still wins over
an older version in a higher-priority root.

On Windows the global cache root is `%LOCALAPPDATA%/Marmot/cache` when
available. On other systems Marmot falls back to `~/.marmot/cache`.

## Resolution And Installation

When the `marmot` tool prepares a project package environment, it:

1. loads the active manifest from `project.marmot`, or from `package.marmot`
   when no project manifest exists
2. parses direct dependencies and version constraints
3. prefers `marmot.lock` when the root manifest checksum still matches and the
   locked package directories are available
4. otherwise scans the local index, resolves the highest compatible versions,
   and vendors them into `packages/<name>-<version>/`
5. writes a new `marmot.lock`
6. writes the build plan's search paths from the resolved package graph and
   project settings

The effective search path order inside a project is:

1. `source_dir`
2. resolved package directories in dependency order
3. `marmot_path` entries from the active manifest
4. `prelude_dir`
5. existing `MARMOT_PATH` entries from the environment

Missing directories are skipped and duplicate paths are removed.

## Vendored Layout

A resolved package is copied into the project-local packages directory using a
versioned folder name.

```text
MyApp/
  project.marmot
  marmot.lock
  packages/
    Greeter-1.2.0/
      package.marmot
      Greeter.mmt
      lib/
        windows/x64/greeter.dll
```

The resolver and lockfile then refer to that vendored directory as the active
package location.

## CLI Workflow

- `marmot install`
  Resolve current dependencies, vendor packages locally, and update
  `marmot.lock`.
- `marmot install <package> [--version <constraint>]`
  Add a direct dependency to the active manifest, then resolve and install.
  If `--version` is omitted, Marmot looks up the highest locally available
  version and writes a caret constraint such as `^1.2.0`.
- `marmot update [package]`
  Force a fresh resolve and rewrite `marmot.lock`. The optional package name is
  currently validated, but the command still refreshes the full dependency
  graph.
- `marmot remove <package>`
  Remove a direct dependency from the active manifest, refresh the lockfile, and
  remove unused vendored directories for that package name.
- `marmot list`
  Print the resolved dependency tree. Marmot prefers `marmot.lock` and falls
  back to a fresh resolve when needed.

All four commands operate on the active manifest from the current directory:
`project.marmot` if present, otherwise `package.marmot`.

## Manifest Format

`package.marmot` is read by the `marmot` tool.

### `[package]`

Recognized fields:

- `name`
- `version`
- `authors`
- `description`
- `license`
- `repository`
- `marmot_version`

Current validation:

- `version` must parse as semantic versioning
- `marmot_version` must parse as a version constraint
- the current compiler version must satisfy `marmot_version`

### `[package.modules]`

Recognized fields:

- `main`
- `exports`

Notes:

- `main` identifies the package entry module
- `exports` is retained as manifest metadata
- actual visibility is still controlled by the module source with
  `public export` and `private export`

### `[dependencies]`

`[dependencies]` is a string table of package name to version constraint.

Supported constraint forms include:

- `^1.2.3`
- `~1.2.3`
- `=1.2.3`
- `>=1.0.0`
- `<2.0.0`
- `>=1.0.0, <2.0.0`

Constraints are validated during manifest load and resolved transitively.

### `[ffi]`

Recognized fields:

- `enabled`
- `library_name`
- `abi_version`
- `thread_safe`

`library_name` is the name the package's source uses for its library:

```marmot
foreign "marmot_image_read_info" ReadInfo : fn(Text) -> Int from "marmot_image";
```

The functions themselves are declared in source, not in the manifest; a
manifest that still has an `[ffi.functions]` table is reported and provides no
library.

Current validation:

- `abi_version` must be a positive integer
- enabled packages must target the current runtime ABI version
- the symbols a program declares are looked up in the loaded library before it
  runs
- at compile time, a `foreign "Name"` declaration without `from` must name a
  builtin runtime function; anything else is the compile error
  `CodeGeneratorUnknownForeignFunction`, rather than a failed call at run time

### `[build]`

Recognized fields:

- `cmake_minimum_version`
- `cpp_standard`

This section is still metadata only. Marmot does not invoke a native build tool
from the manifest today.

### `[prebuilt]`

Recognized platform keys:

- `windows_x64`
- `linux_x86_64`
- `macos_arm64`
- `macos_x86_64`

Each entry contains:

- `path`
- `checksum`

When a matching prebuilt entry exists, Marmot uses that path for the native
library and verifies its checksum before loading.

## Lockfile

The lockfile is `marmot.lock` in the project root.

```toml
# Auto-generated by Marmot. Do not edit manually.
[metadata]
marmot_version = "..."
generated = "2026-04-05T12:00:00Z"
manifest_checksum = "sha256:..."

[[package]]
name = "Greeter"
version = "1.2.0"
source = "local:packages/Greeter-1.2.0"
checksum = "sha256:..."
dependencies = []
```

`manifest_checksum` tracks the active root manifest file. If it changes, Marmot
treats the lockfile as stale and re-resolves.

Each package `checksum` is computed from:

- `package.marmot`
- all `.mmt` files under the package directory

When a locked package directory is missing or its manifest no longer matches the
lockfile entry, Marmot falls back to a fresh resolve. When package source
checksums drift, Marmot emits warnings.

## Native Library Selection And Verification

For FFI-enabled packages, the `marmot` tool chooses the library path like this:

1. use the matching `[prebuilt]` entry for the current platform when present
2. otherwise fall back to the conventional `lib/` path

Fallback paths:

- Windows: `lib/windows/x64/<library_name>.dll`
- macOS: `lib/macos/lib<library_name>.dylib`
- Linux: `lib/linux/x86_64/lib<library_name>.so`

`marmotc run` verifies the selected prebuilt checksum before loading:

- matching checksum: load continues
- checksum mismatch: load fails
- no checksum: load continues with a warning

Marmot does not yet build native libraries automatically when no prebuilt binary
is available.

## Native Packages At Build And Run Time

The `marmot` tool resolves packages before anything compiles. In the build plan
it lists the library of every native package the program can reach (the entry
file's own package and any package that is itself a search path) with its path,
`thread_safe` and checksum.

1. an import resolves to an `.mmt` file on the plan's search paths
2. a `foreign ... from "name"` declaration names a library; the compiler
   records it in the program and its `.mmc`
3. `marmotc run` finds each library the program names, through the plan or a
   library search path, and loads it just before the program starts; `check`
   and `build` never load a library
4. a library that is not found, fails to load, or lacks a declared symbol
   stops the run

See [Build Plans](plan-file.md#native-libraries) for the search order.
