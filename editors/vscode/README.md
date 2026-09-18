# Marmot VSCode Extension

This sample extension provides:

- `.mmt` file association
- TextMate syntax highlighting
- on-save diagnostics powered by `Marmot.exe check <file> --format json`
- quick-fix entries that surface Marmot suggestions

## Local Use

1. Open this folder in VSCode extension development mode.
2. Set `marmot.executablePath` if `Marmot.exe` is not already on your `PATH`.
3. Open a `.mmt` file and save to refresh diagnostics.
