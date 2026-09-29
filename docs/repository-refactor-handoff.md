# Marmot repository refactor: development handoff

## Goal and agreed boundaries

Make `marmotc` and `marmotvm` independently configurable and buildable C++ projects. They must not link the same Marmot library or include each other's headers. Their contract is the versioned `.mmc` file format, so a future OCaml implementation of `marmotc` can replace the C++ one without changing `marmotvm`.

Keep `marmot` as the Rust project tool. It invokes the two programs as processes. Keep the prelude and language test corpus at the repository root during this refactor; moving them would change many relative imports without helping the compiler/VM boundary.

Use executable names for project folders:

```text
Marmot/
├─ projects/
│  ├─ marmotc/          # C++ compiler, CLI, .mmc writer, own CMakeLists.txt
│  ├─ marmotvm/         # C++ VM, .mmc reader, disassembler, own CMakeLists.txt
│  ├─ marmot/           # Rust project tool, Cargo.toml
│  └─ web/              # browser integration of separate compiler and VM builds
├─ format/mmc/          # language-neutral format contract and conformance fixtures
├─ MarmotPrelude/
├─ test/                # end-to-end Marmot programs and snapshots
├─ scripts/             # cross-project orchestration and gate
├─ benchmarks/
├─ editors/
└─ docs/
```

There is no root `CMakeLists.txt` and no installable C++ package shared by `marmotc` and `marmotvm`. Each C++ project may have internal targets within its own build. `scripts/dev.py` remains the convenient front door and invokes separate CMake builds. `format/mmc/` is a contract and fixture directory, not a CMake project or link dependency.

## Current coupling to remove

- The root `CMakeLists.txt` builds `MarmotBytecode`, then links it into both `MarmotCompiler` and `MarmotRuntime`. Its `bytecode/src` directory mixes the in-memory `MidoriExecutable`, artifact reader and writer, opcode and builtin tables, scalar helpers, and disassembler.
- `compiler/src/Utility/CLI/CLI.cpp` writes `.mmc` through `MidoriBinaryArtifact`; `runtime/src/Loader/ProgramLoader.cpp` reads it through the same implementation. Split those responsibilities, including their in-memory models.
- `bytecode/src/Bytecode/Executable/OpCodes.def` and `bytecode/src/Bytecode/Builtins/Builtins.def` are C++ macro lists whose entry positions are serialized IDs. `Format.h` defines `MbcFormatVersion` (currently 13). An OCaml emitter needs a language-neutral definition of those IDs and their encoding.
- The disassembler currently lives in `bytecode` and is called by both compiler diagnostics and VM tracing. It belongs on the reader side. Make `marmotvm disassemble <file.mmc>` a normal feature in every build profile. The compiler should use its own AST/IR printers and write `.mmc` for bytecode inspection.
- `cmake/UnitTests.cmake` creates one binary linked to the compiler and runtime. Split tests by project; keep cross-project tests at the process and `.mmc` boundary.
- `web/src/WASM/WasmWrapper.cpp` directly passes a C++ `MidoriExecutable` from compiler to VM. The browser integration must pass serialized `.mmc` bytes between separate WebAssembly modules or another explicitly versioned byte boundary.

## Migration sequence

Complete each phase with a working checkout before starting the next. Moves, build changes, and format changes should not be combined into one unreviewable step. Do not keep duplicate resolution or serialization paths as permanent fallbacks.

### 1. Freeze and exercise the existing `.mmc` contract

1. Record the exact version-13 header, sections, byte order, opcode numbers and operand widths, builtin indexes and signatures, strings, source metadata, checksums, and malformed-file behavior under `format/mmc/`. Define which changes require a format-version bump. This is documentation of existing behavior, not a new format.
2. Save a small set of `.mmc` fixtures produced by the current `marmotc`: a minimal program, control flow, calls, strings, builtins, source metadata, and an FFI import. Add invalid fixtures for truncation, checksum failure, and incompatible version. Keep fixture provenance and expected VM behavior beside each file. Make a narrow `.gitignore` exception for these fixtures because the repository currently ignores `*.mmc`.
3. Add a conformance check that runs the current VM against those fixtures. Compare newly emitted artifacts with the baseline at the byte level where deterministic; account explicitly for any intentionally variable fields. Do not rewrite expected snapshots to hide a regression.

**Exit:** The existing compiler and VM still pass the gate, and a future writer or reader can be checked against stable version-13 fixtures.

### 2. Separate writer code from reader code

1. Give `marmotc` private opcode and builtin IDs, its own executable representation, and a writer. Move compiler-specific scalar and linking helpers with it. It must not include `marmotvm` headers.
2. Give `marmotvm` private opcode and builtin IDs, its own loaded-program representation, and a reader with format validation. Move the disassembler and VM-side scalar helpers with it. It must not include `marmotc` headers.
3. Keep the shared facts in `format/mmc/` language-neutral. A small machine-readable opcode/builtin registry may generate language-specific constants; generated C++ is checked into, or produced within, each project. Do not make either project compile against a shared C++ header. Keep the future OCaml emitter in mind when choosing the registry format.
4. Delete `MarmotBytecode` after all users have migrated. Update `scripts/testing/layering.py` to reject cross-project includes and links, rather than merely preserving the old `bytecode` layering rule.

**Exit:** `marmotc` writes fixtures that `marmotvm` loads; neither project links a shared Marmot target or includes the other's files. Artifact bytes and language behavior remain unchanged.

### 3. Give each project its own build and tests

1. Move source and unit tests into `projects/marmotc/` and `projects/marmotvm/`. Each owns a top-level `CMakeLists.txt`, project version/settings, build presets, and CTest targets. Internal compiler or VM libraries are permitted but stay inside their project.
2. Move `tool/` to `projects/marmot/` with its `Cargo.toml` and lockfile. Update Cargo paths in scripts and developer commands.
3. Change `scripts/dev.py`, `scripts/lib/presets.py`, build/configure/clean/install scripts, and gate steps to use independent build directories, such as `out/build/marmotc/<profile>` and `out/build/marmotvm/<profile>`. Preserve `dev.py` as the single orchestration command, not a shared CMake build.
4. Split the current combined Catch2 target. Compiler tests may use compiler test support; VM tests may use VM test support. Tests needing both programs become subprocess integration tests over `.mmc` files. Move the native FFI test library to the tests that use it.
5. Remove the root `CMakeLists.txt` and root `CMakePresets.json` only after the separate configure, build, test, install, and clean paths work. Update path references in scripts and existing docs that the moves make wrong.

**Exit:** `cmake -S projects/marmotc -B <build-dir>` and `cmake -S projects/marmotvm -B <build-dir>` each configure and build without the other project's source or build tree. `cargo test --manifest-path projects/marmot/Cargo.toml` passes. `python scripts/dev.py build`, `test`, `run`, and `gate` still work.

### 4. Rework build profiles and diagnostic features

Expose `Debug`, `Dev`, and `Release` across both C++ projects. Use conventional CMake configuration behavior where practical (`Dev` may map to `RelWithDebInfo` in the orchestration script); keep profile names consistent in `dev.py`.

| Profile | Native build | Internal verification | Diagnostics |
|---|---|---|---|
| Debug | No optimization, full symbols | Expensive VM checks; compiler IR verification after each pass | Trace and metrics compiled in, emitted only on request |
| Dev | Optimized, with symbols | Compiler IR verification after each pass | Metrics compiled in, emitted only on request; default for development |
| Release | Highest portable, semantics-preserving optimization; LTO when supported | Compiler IR verification before backend; required VM validation | Normal user features and errors; no unsolicited internal output |

1. Make `marmotc --emit-ast`, `--emit-ir`, and `marmotvm disassemble <file.mmc>` available in all profiles. Use `marmotvm run <file.mmc>` for execution and update its callers and CLI contract together. A printer or disassembler is a product capability, not a build mode.
2. Provide explicit diagnostic switches for optimizer statistics, opcode metrics, and execution tracing where compiled in. Send internal diagnostics to stderr and keep normal stdout and JSON contracts clean. Debug/Dev may compile extra instrumentation but should not print metrics merely because of their profile.
3. Keep the same Marmot language optimization pipeline and `.mmc` format across profiles. Native C++ optimization and checking intensity may differ; generated program semantics may not.
4. Retire `Experimental` as a profile. If useful, expose AVX2 or other CPU tuning as explicit platform-specific options and benchmark them separately. Do not fold fast-math into Release because it may change Float behavior.
5. Keep all compile definitions that affect VM value layout consistent across every target inside `marmotvm` and its tests. The current `MIDORI_DEBUG_FULL` changes `sizeof(MidoriValue)`; preserve the ABI consistency test while reorganizing.

**Exit:** Each profile builds both projects; the AST/IR and disassemble commands work in Release; optional metrics do not appear in ordinary program output; the profile matrix passes targeted tests.

### 5. Move browser integration to the same boundary

Replace the current single WebAssembly module that links compiler and VM C++ code with independently built compiler and VM modules. The browser adapter passes `.mmc` bytes and structured output between them. Keep the existing playground behavior and deployment command, updating artifact names and site integration together. Do not introduce a browser-only in-memory executable interchange.

**Exit:** A browser smoke test compiles a program, transfers `.mmc` bytes, runs it, and reports compiler and runtime errors. The compiler and VM WebAssembly builds do not link each other.

### 6. Final integration and removal

Update `marmot` executable discovery, install layout, `MARMOT_PATH` handling, VS Code integration, benchmarks, and existing documentation for the new paths and `marmotvm` command shape. Remove unused old source directories, old presets, obsolete macros, and any temporary adapters. Keep the prelude import paths and existing language snapshots intact unless a reviewed behavior change requires an update.

Run the full `python scripts/dev.py gate` before any commit, as required by `AGENTS.md`. Also run separate CMake configure/build/CTest for `marmotc` and `marmotvm` in Debug, Dev, and Release; Cargo tests; `.mmc` fixture compatibility tests; and the WebAssembly smoke test where Emscripten is available. Do not commit or push unless asked.

## Final acceptance checklist

- `marmotc` can be built, tested, and installed from its own directory without configuring or linking `marmotvm`; the reverse holds for `marmotvm`.
- No C++ library, header, or in-memory executable type crosses the compiler/VM boundary. Their only program interchange is versioned `.mmc` bytes.
- The format contract contains enough information to implement an OCaml writer, and independent fixtures catch opcode, builtin-index, and serialization drift.
- `marmotvm disassemble` works in Release. AST and IR printers remain available in Release. Internal metrics are explicit and do not pollute program stdout.
- `marmot`, native scripts, installation, language suite, and browser workflow use the new project paths and pass their checks.
- The root has no CMake project; `Experimental` and the old shared bytecode target are gone.
