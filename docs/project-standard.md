# Project Standard

This document defines the standard layout and manifest for a Marmot project or
package workspace.

## Active Manifest

The `marmot` tool looks for `project.marmot` first, walking up from the file
it compiles (or from the current directory for project commands).

If no project manifest exists, a package root with `package.marmot` is treated
as the active manifest instead. In that mode:

- the workspace name comes from `[package].name`
- the entry file comes from `[package.modules].main`
- `source_dir` defaults to `.`
- `packages_dir` defaults to `packages`
- `prelude_dir` defaults to `MarmotPrelude`

## Manifest

Example:

```toml
[project]
name = "MyApp"
entry = "src/Main.mmt"
source_dir = "src"
packages_dir = "packages"
prelude_dir = "MarmotPrelude"
marmot_path = ["registry", "../shared-packages"]

[dependencies]
Greeter = "^1.2.0"
Image = ">=0.4.0, <0.5.0"

[test]
dir = "test"
timeout_ms = 30000
```

### Fields

- `name` (optional): display name for tools
- `entry` (optional): entry source file for tooling
- `source_dir` (optional): source directory for project modules. When omitted,
  Marmot uses the conventional `src` directory.
- `packages_dir` (optional): project-local vendored package directory. This is
  where `marmot install` writes resolved packages as
  `packages/<name>-<version>/`. When omitted, Marmot uses the conventional
  `packages` directory.
- `prelude_dir` (optional): directory containing `MarmotPrelude`. When omitted,
  Marmot uses the conventional `MarmotPrelude` directory.
- `marmot_path` (optional): extra search paths and package index roots, relative
  to the project root unless absolute
- `[dependencies]` (optional): top-level table of direct package dependencies,
  mapping package names to version constraints
- `[test].dir` (optional): test directory used by `marmot test`. Defaults to
  `test`.
- `[test].timeout_ms` (optional): per-test timeout budget enforced by
  `marmot test`. Defaults to `30000`.

Dependency constraints use the same syntax as `package.marmot`, including caret,
tilde, exact, comparison, and compound ranges.

## Layout

```text
MyApp/
  project.marmot
  marmot.lock
  src/
  test/
  packages/
  MarmotPrelude/
  native/
  README.md
```

`marmot.lock` is written after dependency resolution. `packages/` contains the
vendored package sources that back the current local package workflow.

## Package Workflow

Projects declare direct dependencies in the top-level `[dependencies]` table.
You can edit that table manually or use the CLI:

- `marmot install`
- `marmot install <package> [--version <constraint>]`
- `marmot update [package]`
- `marmot remove <package>`
- `marmot list`

`marmot install` resolves direct and transitive dependencies, vendors the chosen
versions into `packages/`, and writes `marmot.lock`.

Because remote registry fetching is not implemented yet, reproducible dependency
setups currently depend on keeping the vendored `packages/` directory or some
other local package root available.

## Module Naming

Module names should mirror paths relative to `source_dir`:

`src/Foo/Bar.mmt` -> `module Foo.Bar`

If you keep modules at the project root, set `source_dir = "."`.

## Search Paths

Inside a project, the `marmot` tool gives the compiler its search paths in a
build plan, built from the resolved package graph and the active manifest, in
this order:

1. `source_dir`
2. resolved package directories in dependency order
3. `marmot_path` entries from the active manifest
4. `prelude_dir`
5. existing `MARMOT_PATH` entries from the environment

Existing environment entries are appended after project-managed paths, duplicate
paths are removed, and missing directories are skipped.

`packages_dir` is no longer treated as a single flat import root during project
resolution. Instead, Marmot resolves packages first and then adds each selected
package directory to the search path individually.

## Project Initialization

Use the CLI to scaffold a project:

```text
marmot init [path] [--name <project_name>]
```

This creates:

- `project.marmot`
- `src/Main.mmt`
- `test/`
- `packages/`
