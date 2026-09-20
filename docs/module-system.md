# Module System

Marmot modules provide namespace isolation, explicit visibility, and dependency-driven multi-file compilation.

## Core Rules

Every `.mmt` file must satisfy these rules:

- It must contain exactly one explicit `module` declaration.
- That `module` declaration must be the first top-level statement in the file.
- Leading whitespace and comments are allowed before `module`.
- Any top-level code, `import`, `use`, or export block before `module` is rejected.

After the `module` declaration, top-level module statements are flexible:

- `import` may appear anywhere at top level.
- `use` may appear anywhere at top level.
- `public export` and `private export` may appear anywhere at top level.
- Multiple import blocks and multiple export blocks are allowed.

`ModuleManager` scans these statements before ordinary parsing, so their placement after `module` is a source-layout choice rather than a semantic phase boundary.

## Syntax

### Module Declaration

```marmot
module Math.Vector
```

Module names use dot-separated identifiers and define the qualification prefix used by `::`.

### Export Blocks

```marmot
module Math.Vector
public export { add, dot }
private export { debug_helper }
```

Visibility levels:

- `public export`: accessible to all importers
- `private export`: accessible only to modules that share the same namespace prefix
- unexported symbols: module-internal only

### Import Forms

System import through `MARMOT_PATH`:

```marmot
import { <IO> }
import { <Math.Vector> }
```

Path import relative to the importing file:

```marmot
import { "./helpers.mmt" }
import { "../lib/database.mmt" }
```

Multiple imports can share a block:

```marmot
import { <IO>, "./helpers.mmt" }
```

### Use Form

```marmot
use Math.Vector.{add, multiply}
```

One name is written the same way: `use Math.Vector.{add}`.

Without `use`, cross-module access stays qualified:

```marmot
def result = Math.Vector::add(v1, v2);
```

A bare name is a local binding or one a `use` brought into scope. Importing a
module does not put its names in scope on their own: a name that only an
imported module exports is an error that says which modules export it, and how
to reach it. Two imported modules exporting the same name therefore collide only
if you `use` both, which is an error naming both.

The rule is the same for type names and for a union's constructors. A type an
imported module exports is written `Shapes::Point` or brought in with
`use Shapes.{Point}`; the constructors of a union come into scope with the union
itself, so `use Result.{Result}` is what makes `Result::Ok` mean something.

## Flexible Placement

Only `module` is fixed in position. Other module statements can be scattered:

```marmot-test name=module-system/flexible_placement path=.doc_examples/module_system/flexible_placement.mmt
module Example

def LocalHelper = fn(x: Int) -> Int => x + 1;

import { <IO> }

public export { main }

use IO.{PrintLine}

def main = fn() -> Int => {
    PrintLine((LocalHelper(41)) as Text);
    0
};
```

This matches the current implementation and the regression fixtures under `test/module/success/`.

## Resolution Behavior

### Import Resolution

`ImportResolver` resolves:

- `<Module.Name>` by converting it to `Module/Name.mmt` and trying each search path in order
- `"relative/path.mmt"` relative to the importing file

The compiler does not find the search paths itself; it is given them. For a
file inside a project (a `project.marmot` or `package.marmot` above it), the
`marmot` tool passes, in a [build plan](plan-file.md), the project's source
directory, its resolved dependencies, its extra paths, its prelude directory,
then `MARMOT_PATH`. Outside a project, and for `marmotc` given a file on its
own, they are `MARMOT_PATH` alone.

Platform notes:

- `MARMOT_PATH` uses `;` on Windows and `:` on Unix-like systems.
- Search paths that do not exist are skipped.
- Resolved import paths are normalized to absolute paths.

### Build Graph Construction

`ModuleManager` recursively loads imported modules and builds a `BuildGraph` containing:

- a stripped token stream for each module body
- source lines for later diagnostics
- dependency edges
- collected `use` imports
- module declarations and export metadata
- a module-name-to-file map for duplicate detection

Errors raised here include:

- unresolved import
- import file open failure
- circular dependency
- missing module declaration
- duplicate module declaration
- duplicate module name across files

### Duplicate Module Names

Two different files cannot declare the same module name. The build graph rejects the second declaration before parsing proceeds.

## Typeclass Instances

An instance belongs in the module that declares its class or the module that
declares its type. Two imported modules declaring an instance for the same type
is an error naming both: which one a call used would otherwise depend on the
order of imports. The same instance reached through several import paths is one
instance, not a conflict.

## Symbol Visibility

### Qualified Access

Cross-module names use `::`:

```marmot-test name=module-system/qualified_access path=.doc_examples/module_system/qualified_access.mmt module=ModuleQualifiedAccess
import { <IO> }

def main = fn() -> Int => {
    IO::PrintLine("Hello");
    0
};
```

### Unqualified Access via `use`

```marmot-test name=module-system/use_access path=.doc_examples/module_system/use_access.mmt module=ModuleUseAccess
import { <IO> }
use IO.{PrintLine}

def main = fn() -> Int => {
    PrintLine("Hello");
    0
};
```

### Privacy

Private exports are visible only when the importer shares the same namespace prefix.

Examples:

- `Math.Vector` can access private exports from `Math.Internal`
- `App.Main` cannot access private exports from `Math.Internal`

### Exported Types

Types must be exported to be used from another module through qualified access.

```marmot
module MyLib
public export { PublicType, GetValue }

type PublicType =
{
    value: Int
};

type InternalType =
{
    value: Int
};
```

`MyLib::PublicType` is visible to importers. `MyLib::InternalType` is not.

When a union type is exported, its constructors become available with it.

## Compilation Scheduling

The module system computes stable tiers, but compilation itself is dependency-driven.

Current behavior in `Compiler.cpp`:

- `BuildGraph::GetCompilationTiers()` is used for deterministic progress output and final linking order.
- The compiler separately tracks remaining dependency counts for each module.
- Modules whose dependencies are satisfied are pushed into a ready queue immediately.
- Native builds use a pool of `std::jthread` workers to consume the queue.
- Emscripten builds use the same dependency logic with a single-threaded deque.

This means tiers are metadata for reporting and stable ordering, not a hard "finish tier N before starting tier N+1" execution barrier.

## Per-Module Compilation

After module resolution, each module is compiled through:

1. parse
2. type-signature extraction for exported API
3. type checking
4. static analysis
5. optimization
6. code generation

Each completed module contributes:

- exported symbol visibility
- exported type signatures
- typeclass metadata
- warnings
- optional bytecode ready for linking

## Linking

`BytecodeLinker` combines all compiled modules into a single executable by:

- assigning global procedure and global-variable offsets
- collecting exports
- checking duplicate exported symbols
- resolving imports and patching bytecode
- concatenating procedures
- generating the bootstrap entry path

The declared entry module name is preserved for bootstrap and debug labeling when available.

## Package Interaction

A module names the native library its foreign functions come from (`foreign ... from "library"`), and the compiler records the library and the module's directory in the program. `marmotvm` loads the library just before the program starts; `marmotc` never loads it. See [Package System](package-system.md) and [Build Plans](plan-file.md#native-libraries).
