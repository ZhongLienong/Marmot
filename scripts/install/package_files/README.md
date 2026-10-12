# Marmot

`bin` holds `marmot`, the project tool, with the `marmotc` compiler and the
`marmotvm` VM it drives. `MarmotPrelude` is the prelude that `<IO>` and the other
`<Name>` imports resolve against.

Run the setup script once from this folder, then open a new terminal:

- Linux: `./install.sh`, and add the line it prints to your shell profile.
- Windows: `powershell -ExecutionPolicy Bypass -File install.ps1`

It puts `bin` on `PATH` and `MarmotPrelude` on `MARMOT_PATH`. After moving the
folder, run it again.
