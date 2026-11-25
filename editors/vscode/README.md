# Marmot Language Support for VS Code

- Syntax highlighting, comments, and bracket pairing for `.mmt` files.
- **Format Document** and opt-in **Format on Save** using the Marmot formatter.
- Compiler diagnostics in Problems when you open or save a file.
- Quick Fix entries that display suggestions supplied by the compiler.
- **Marmot: Run File** and **Marmot: Run Project** in the integrated terminal.

## Install

In VS Code, run **Extensions: Install from VSIX…** and select the
`marmot-lang-0.3.0.vsix` package.

To build the package, run these PowerShell commands from the Marmot repo root.
The VSIX will be written to `editors/vscode`.

```powershell
Set-Location .\editors\vscode
npx.cmd @vscode/vsce package --skip-license
```

## Toolchain

Formatting, diagnostics, and run commands use the Marmot project tool in trusted workspaces.
Install the Marmot toolchain and put `marmot` on `PATH`
or set `marmot.executablePath` to the full path of the project tool. The tool
is in `<install-dir>/bin/` for an installation or under
`projects/marmot/target/debug/` or `projects/marmot/target/release/` for a
checkout build.

**Format Document** formats the current editor contents, including unsaved changes.
To format when saving a Marmot file, add this to your VS Code settings.

```json
{
  "[marmot]": {
    "editor.defaultFormatter": "marmot.marmot-lang",
    "editor.formatOnSave": true
  }
}
```

If formatting or diagnostics fail, open **View → Output** and select **Marmot** for
details. Syntax highlighting also works in Restricted Mode.

## Running Marmot

Use **Marmot: Run File** from the Command Palette or the play button in a
Marmot editor. The command saves the file and runs it from its containing
directory.

Open the folder containing your Marmot manifest and run **Marmot: Run Project**
to run its entry file. In a multi-root workspace, the command uses the active
editor's workspace folder or asks you to choose one. Both commands show output
and accept program input in the integrated terminal.
