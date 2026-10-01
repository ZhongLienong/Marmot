# Marmot VSCode Extension

This sample extension provides:

- `.mmt` file association
- TextMate syntax highlighting
- on-save diagnostics powered by `marmot check <file> --format json`
- quick-fix entries that surface Marmot suggestions

## Local Use

1. Open this folder in VSCode extension development mode.
2. Set `marmot.executablePath` if `marmot` is not already on your `PATH`. An installed tool is in `<install-dir>/bin/`; a checkout build is in `projects/marmot/target/debug/` or `release/`.
3. Open a `.mmt` file and save to refresh diagnostics.
