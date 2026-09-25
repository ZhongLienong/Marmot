#!/usr/bin/env python3
"""
Remove an install made by install.py: its files, and its MARMOT_PATH and PATH entries.

On Windows the entries are taken out of the registry. Elsewhere they live in
the install's env.sh, which goes with the files; the line sourcing it from the
shell profile is left for you to delete.

Examples:
    python scripts/dev.py uninstall
    python scripts/dev.py uninstall --keep-files
    python scripts/dev.py uninstall --install-dir ~/opt/marmot
"""

from __future__ import annotations

import argparse
import shlex
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from install import layout
from install.layout import InstallLayout
from lib import console
from lib.host import IS_WINDOWS


def remove_files(target: InstallLayout, force: bool) -> None:
    if not target.root.exists():
        console.note(f"nothing at {target.root}")
        return
    if target.root == Path(target.root.anchor):
        raise SystemExit(f"Refusing to remove a filesystem root: {target.root}")
    if not force and not target.marker.is_file():
        raise SystemExit(f"{target.root} has no {layout.MARKER_FILENAME}, so it may not be a Marmot install. Pass --force to remove it anyway.")
    shutil.rmtree(target.root)
    console.ok(f"removed {target.root}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Remove a Marmot install and its environment entries.")
    parser.add_argument("--scope", choices=["user", "machine"], default="user", help="Windows: the variables of the user or the machine (default: user).")
    parser.add_argument("--install-dir", default="", help="The install to remove (default: %%LOCALAPPDATA%%\\Marmot, or ~/.local/share/marmot).")
    parser.add_argument("--keep-files", action="store_true", help="Only remove the environment entries.")
    parser.add_argument("--force", action="store_true", help="Remove the folder even without the install marker.")
    args = parser.parse_args(argv)

    layout.require_scope_allowed(args.scope)
    target = InstallLayout.resolve(args.scope, args.install_dir or None)

    if IS_WINDOWS:
        layout.update_registry_list(args.scope, "MARMOT_PATH", lambda value: layout.without_entry(value, target.prelude_dir))
        layout.update_registry_list(args.scope, "PATH", lambda value: layout.without_entry(value, target.bin_dir))
        layout.broadcast_environment_change()
        console.ok("removed the MARMOT_PATH and PATH entries")
    else:
        print(f"Delete this line from your shell profile if you added it:\n  . {shlex.quote(str(target.env_script))}")

    if not args.keep_files:
        remove_files(target, args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
