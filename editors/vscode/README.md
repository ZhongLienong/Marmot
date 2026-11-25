# Marmot Language Support for VS Code

- Syntax highlighting, comments, and bracket pairing for `.mmt` files.
- Compiler diagnostics in Problems when you open or save a file.
- Quick Fix entries that display suggestions supplied by the compiler.

## Install

In VS Code, run **Extensions: Install from VSIX…** and select the
`marmot-lang-0.1.0.vsix` package. To build the package from this directory,
run `npx @vscode/vsce package --skip-license`.

## Diagnostics

The extension runs `marmot check <file> --format json` on saved source in
trusted workspaces. Install the Marmot toolchain and put `marmot` on `PATH`
or set `marmot.executablePath` to the full path of the project tool. The tool
is in `<install-dir>/bin/` for an installation or under
`projects/marmot/target/debug/` or `projects/marmot/target/release/` for a
checkout build.

If a check cannot run, open **View → Output** and select **Marmot** for
details. Syntax highlighting also works in Restricted Mode.
