# Marmot Repo Instructions

These instructions apply to the entire repository unless a deeper `AGENTS.md`
overrides them.

## What is here

Marmot is a language with three binaries:

- `marmotc` — the compiler. Reads `.mmt` source, writes a `.mmc` program. It
  never runs anything.
- `marmotvm` — the VM. Runs a `.mmc`.
- `marmot` — the project tool, written in Rust (`tool/`). Resolves projects and
  packages and drives the other two through a build plan.

Layering, enforced by `scripts/testing/layering.py`: `common` → `runtime` and
`compiler` → the driver (`compiler/src/Utility/{CLI,Driver}`) → `vm/src`. The
driver does not link the runtime.

| Path | What it holds |
|---|---|
| `common/src` | Values, opcodes, the executable format, errors, builtin table |
| `compiler/src/Compiler` | Lexer, parser, type checker, static analysis, optimizer, code generator, linker |
| `runtime/src` | The interpreter, the GC, workers, the builtin FFI library |
| `vm/src` | The `marmotvm` entry point |
| `tool/src` | The `marmot` tool |
| `MarmotPrelude` | The prelude, written in Marmot |
| `test/` | The `.mmt` regression suite, with `.expected` snapshots |
| `docs/` | The language and toolchain documentation |

## Build and test

`scripts/dev.py` is the front door, the same on Windows and Linux; it loads
MSVC's environment itself on Windows and picks a new enough GCC or Clang on
Linux. `python scripts/dev.py` lists the commands.

```
python scripts/dev.py build                 # marmotc and marmotvm, Development
python scripts/dev.py gate                  # the full gate
python scripts/dev.py test --category module
python scripts/dev.py run scratch.mmt
```

The gate is what "green" means here: layering, the build, C++ unit tests, doc
examples, CLI contracts, formatting, benchmarks, the tool's cargo tests and the
language suite, stopping at the first failure. `--skip` and `--only` take step
names. Commands that need a build bring it up to date first.

The language suite through the tool, in about thirteen seconds:

```
cargo run --quiet --manifest-path tool/Cargo.toml -- test
cargo run --quiet --manifest-path tool/Cargo.toml -- test module/      # one folder
```

The scripts are grouped by purpose under `scripts/`: `make/` (doctor, configure,
build, clean, wasm), `testing/` (the gate and each check), `program/` (run,
fmt), `bench/`, `install/`, with shared helpers in `lib/`. Each runs on its own
too, e.g. `python scripts/testing/language.py --category closure`.

On Linux the sources need GCC 14+ or Clang 19+ (GCC 13 has no `<print>`, and
Clang 18 cannot use libstdc++'s `std::expected`). `CXX` overrides the compiler
the scripts pick; a build tree keeps the compiler it was configured with, so
switch with `dev.py configure --fresh`.

Run the gate before committing. Do not commit or push unless asked.

## Rules that are not style

- **No defensive programming.** When a construct is removed, its handling goes
  with it. Do not keep a fallback for a case the compiler already rejects, and
  do not add a second resolution path "just in case" — several bugs here came
  from exactly that (two mangling paths, two equality switches, a bare-name
  fallback beside a qualified lookup).
- **The grammar is sealed at v1.** `docs/grammar.md` is a contract: nothing in
  it is removed, and later versions only add. Do not change the grammar without
  being asked to.
- **Do not write documentation unless asked.** Updating a doc that a change
  makes wrong is part of the change, not new documentation.
- **Tests are snapshots.** A `.mmt` under `test/` passes when its exit status
  matches its place (a file under a `failure/` folder must fail) and its output
  matches the `.expected` beside it, when there is one. Never edit a snapshot to
  match a regression — read the diff and decide which side is wrong.

## Writing Marmot

Agents write Marmot when adding tests or touching the prelude. The rules most
often got wrong:

- **There is no entry function.** A program is its top-level statements, run in
  order. A definition named `main` is an ordinary definition and never runs.
- **A function may name a definition that comes after it**, which is how two
  functions call each other. Nothing is hoisted: a statement that runs may only
  use definitions that have already run, including through the functions it
  calls, and the compiler checks that. A definition used before the checker
  reaches it needs its type written on it.
- **Types and instances may be declared in any order.** A type may name one
  declared below it, and two types may hold each other; generic ones that do
  use the same parameter names (`Tree<T>` and `Forest<T>`).
- **Iteration is recursion.** A call in tail position — the last thing a
  function does, through `if` branches, `match` arms and a block's final
  expression — runs without a new frame, whatever it calls. Any other
  recursion overflows the stack at about 500,000 calls.
- **Mixing operators of different precedence needs parentheses**:
  `(index >= 0) && (index < #items)`.
- **`as` binds tighter than binary operators**: `n as Text ++ "!"` converts `n`;
  converting a whole expression needs parentheses, `(a + b) as Text`.
- **A bare name is local or `use`d.** Anything else is qualified —
  `Module::name`, `Module::Type`, `Module::Union::Member`. This holds for
  values, types and constructors alike.
- **A type's identity includes its module.** Two modules may each declare a
  `Point`, and those are two types.
- `Text` to a number is strict: the whole text must be the number, or the
  program stops with `InvalidConversion`. `TextUtil::ParseInt` and `ParseFloat`
  ask without stopping.
- A `Float` prints as the shortest text that reads back as the same value, with
  `.0` kept on a whole number.
- Building text with `++` and arrays with `ArrayUtil::WithAppended` is
  quadratic. Fine for small inputs, not for a loop over a large one.

Tests under `test/` import the prelude by relative path
(`"../../../MarmotPrelude/IO.mmt"`). The `<IO>` form resolves through
`MARMOT_PATH`, which on a developer machine may point at an installed prelude
rather than this one.

## Compiler invariants

Worth knowing before editing the front end, because each was a bug once:

- A name resolves by **one** rule. There is no ambient fallback that scans every
  import.
- `MidoriType::ToString()` is the **identity** string — it carries the module,
  and `InstanceKey` and `MangleInstanceMethodName` key on it. `DisplayString()`
  is what a diagnostic shows; it qualifies only when both sides would read the
  same.
- Generic functions are keyed by bare name for this module's own and by
  `Module::name` for imported ones. A qualified call is generic only if that
  module's function is.
- `==` and `!=` are emitted by one function, so their type cases cannot drift.
- Every top-level definition gets its global slot before any body is emitted.
  `m_global_variables[name]` on an unknown name would insert slot 0.

## Gotchas

- **Shell heredocs mangle backslashes.** Writing C++, Python or Marmot through
  `bash <<'EOF'` corrupts `\n`, `\"` and `\\`. Use a file-writing tool.
- **Some tools interpret `\uXXXX`** in file content. For a snapshot containing
  escapes, generate the file from verified program output instead of typing it.
- `git add` exits non-zero when a pathspec includes tracked files under an
  ignored `build*` directory. Add those with `-f`, in their own call, and check
  `git status --short` afterwards.
- A developer's `MARMOT_PATH` usually points at the installed prelude in
  `AppData\Local\Marmot`. A debug build of `marmot` prefers its own checkout's
  compiler and prelude; an installed one uses whatever was last installed.
  Reinstall with `python scripts/dev.py install --rebuild`.
- Adding a `RuntimeErrorCode` means touching `Common/Error/Error.{h,cpp}` and the
  lists in `docs/diagnostic-format.md` and `docs/error-reporting.md`.
- Doc examples marked `marmot-test` are mirrored into `test/doc_examples/`. After
  editing one, run `python scripts/dev.py check docs --sync`.

## Finding bugs

Targeted tests miss what real programs hit. The bugs found here — `!=` on text
comparing addresses, a qualified call running another module's generic, a
definition reading its own uninitialised slot — were invisible in the source and
turned up when a few hundred lines of ordinary Marmot ran.

When something looks wrong, write the smallest program that would expose it, run
it, and read the code only after you have seen the behaviour. Then keep that
program as a fixture: every fix above has one, and each fails on the compiler
built before it.

## C++ Style

- Prefer explicit types over `auto` unless the type is obvious and repeating it
  would hurt readability.
- Use C++23 features when they improve clarity and fit the existing codebase.
- Use the `m_` prefix for instance members and the `s_` prefix for static
  values.
- Use PascalCase for class, struct, and function names.
- Prefer defining and using constructors instead of ad hoc initialization
  patterns.
- Prefer putting implementations in `.cpp` files when practical.

## Control Flow

- Always use braces for `if`, `for`, `while`, and similar statements.
- Do not use one-line control-flow bodies without braces.

## Code Shape

- Favor function chaining and a functional style when it keeps the code clear.
- Avoid comments for self-explanatory code.
- Comments say why, not what. A comment that restates the line below it is noise;
  one that records the mistake a line prevents is worth keeping.
- Do not write documentation unless explicitly requested.
