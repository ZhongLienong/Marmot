#!/usr/bin/env python3
"""
Install Marmot from this checkout: marmotc, marmotvm, the marmot tool and the prelude.

The binaries go to <install dir>/bin and the prelude to <install dir>/MarmotPrelude,
and MARMOT_PATH and PATH are pointed at them: in the registry on Windows (a new
terminal sees them), and elsewhere through <install dir>/env.sh, which the
shell profile sources.

The compiler comes from the build given by --build/--preset, else from the one
built last. The default install dir is %LOCALAPPDATA%\\Marmot on Windows and
~/.local/share/marmot elsewhere.

Examples:
    python scripts/dev.py install
    python scripts/dev.py install --build Release --rebuild
    python scripts/dev.py install --prelude-only
    python scripts/dev.py install --install-dir ~/opt/marmot
    python scripts/dev.py install --scope machine        # Windows, as Administrator
"""

from __future__ import annotations

import argparse
import json
import shlex
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from install import layout
from install.layout import InstallLayout
from lib import cargo, console
from lib.host import IS_WINDOWS, PRELUDE_DIR, executable
from lib.presets import BuildTree, add_build_arguments, newest_built


def chosen_compiler(args: argparse.Namespace) -> Path:
    if args.marmot_exe:
        return Path(args.marmot_exe).expanduser().resolve()
    if args.build or args.preset:
        return BuildTree.select(args.build or "Release", args.preset).require_compiler()
    newest = newest_built()
    if newest is None:
        raise SystemExit("No build of marmotc found. Build one with: python scripts/dev.py build --build Release")
    return newest.compiler


def rebuild(args: argparse.Namespace) -> int:
    tree = BuildTree.select(args.build or "Release", args.preset)
    built = tree.build(["marmotc", "marmotvm"])
    if built != 0 or not cargo.is_available():
        return built
    return cargo.build(release=True)


def copy_binaries(target: InstallLayout, compiler: Path) -> None:
    target.bin_dir.mkdir(parents=True, exist_ok=True)
    vm = compiler.with_name(executable("marmotvm"))
    for binary in (compiler, vm):
        if not binary.is_file():
            raise SystemExit(f"{binary} is not built.")
        shutil.copy2(binary, target.bin_dir / binary.name)
        print(f"  {binary.name:<14} from {binary}")

    tool = cargo.newest_binary()
    if tool is None:
        console.note("  the marmot tool is not built; build it with: python scripts/dev.py build tool")
        return
    if cargo.is_stale(tool):
        console.note("  the marmot tool is older than its source; rebuild it with: python scripts/dev.py build tool")
    shutil.copy2(tool, target.bin_dir / tool.name)
    print(f"  {tool.name:<14} from {tool}")


def copy_prelude(target: InstallLayout) -> None:
    if target.prelude_dir.exists():
        shutil.rmtree(target.prelude_dir)
    shutil.copytree(PRELUDE_DIR, target.prelude_dir)
    print(f"  MarmotPrelude  to {target.prelude_dir}")


def write_env_script(target: InstallLayout, with_binaries: bool) -> None:
    lines = [f"export MARMOT_PATH={shlex.quote(str(target.prelude_dir))}\"${{MARMOT_PATH:+:$MARMOT_PATH}}\""]
    if with_binaries:
        lines.append(f"export PATH={shlex.quote(str(target.bin_dir))}\":$PATH\"")
    target.env_script.write_text("# Written by scripts/install/install.py.\n" + "\n".join(lines) + "\n", encoding="utf-8")


def persist_environment(target: InstallLayout, scope: str, with_binaries: bool) -> None:
    if IS_WINDOWS:
        layout.update_registry_list(scope, "MARMOT_PATH", lambda value: layout.with_entry(value, target.prelude_dir))
        if with_binaries:
            layout.update_registry_list(scope, "PATH", lambda value: layout.with_entry(value, target.bin_dir))
        layout.broadcast_environment_change()
        print("\nMARMOT_PATH and PATH are set; open a new terminal to use them.")
        return

    write_env_script(target, with_binaries)
    source_line = f". {shlex.quote(str(target.env_script))}"
    profile = Path.home() / ".profile"
    if profile.is_file() and source_line in profile.read_text(encoding="utf-8"):
        print(f"\n{profile} already sources {target.env_script}; open a new terminal to use it.")
        return
    print(f"\nTo put Marmot on MARMOT_PATH and PATH, add this line to your shell profile (e.g. {profile}):\n  {source_line}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Install Marmot from this checkout.")
    add_build_arguments(parser, default=None)
    parser.add_argument("--scope", choices=["user", "machine"], default="user", help="Windows: set the variables for the user or the machine (default: user).")
    parser.add_argument("--install-dir", default="", help="Where to install (default: %%LOCALAPPDATA%%\\Marmot, or ~/.local/share/marmot).")
    parser.add_argument("--marmot-exe", default="", help="Install this marmotc (and the marmotvm beside it) instead of a build's.")
    parser.add_argument("--prelude-only", action="store_true", help="Install just the prelude and MARMOT_PATH.")
    parser.add_argument("--rebuild", action="store_true", help="Build marmotc, marmotvm and the tool first (Release unless --build says).")
    args = parser.parse_args(argv)

    layout.require_scope_allowed(args.scope)
    if args.rebuild:
        rebuilt = rebuild(args)
        if rebuilt != 0:
            return rebuilt
        args.build = args.build or "Release"

    target = InstallLayout.resolve(args.scope, args.install_dir or None)
    print(f"Installing to {target.root}")
    target.root.mkdir(parents=True, exist_ok=True)
    with_binaries = not args.prelude_only
    if with_binaries:
        copy_binaries(target, chosen_compiler(args))
    copy_prelude(target)
    marker = {"schema_version": 1, "installed_by": "scripts/install/install.py", "scope": args.scope,
              "prelude_dir": str(target.prelude_dir), "bin_dir": str(target.bin_dir)}
    target.marker.write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")

    persist_environment(target, args.scope, with_binaries)
    console.ok("installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
