#!/usr/bin/env python3
"""
Package this checkout's build for this platform: marmotc, marmotvm, the marmot
tool and the prelude, in the layout `dev.py install` writes, with a setup script
that puts the extracted folder on PATH and MARMOT_PATH.

The archive is out/package/marmot-<platform>-x64.zip on Windows and .tar.gz
elsewhere, which keeps the binaries executable. It is extracted and run once
before it is reported, so a package that does not work is not written.

Example:
    python scripts/dev.py package
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from install.install import copy_prelude
from install.layout import InstallLayout
from lib import cargo, console
from lib.host import IS_WINDOWS, REPO_ROOT
from lib.presets import BuildTree, add_build_arguments

PACKAGE_DIR = REPO_ROOT / "out" / "package"
TEMPLATE_DIR = Path(__file__).resolve().parent / "package_files"
SETUP_SCRIPT = "install.ps1" if IS_WINDOWS else "install.sh"
HELLO = 'module Hello\nimport { <IO> }\nIO::PrintLine("hello from the package");\n'


def stage(tree: BuildTree, target: InstallLayout) -> None:
    if target.root.exists():
        shutil.rmtree(target.root)
    target.bin_dir.mkdir(parents=True)
    for binary in (tree.require_compiler(), tree.vm, cargo.binary("release")):
        shutil.copy2(binary, target.bin_dir / binary.name)
    copy_prelude(target)
    for template in (SETUP_SCRIPT, "README.md"):
        shutil.copy2(TEMPLATE_DIR / template, target.root / template)


# Runs a program with the extracted package alone: the Linux setup script's
# env.sh, or on Windows the variables its setup script would set, which are not
# set here because that would change this machine's user environment.
def smoke_test(archive: Path, name: str) -> bool:
    with tempfile.TemporaryDirectory() as scratch:
        shutil.unpack_archive(archive, scratch)
        root = Path(scratch) / name
        (root / "Hello.mmt").write_text(HELLO, encoding="utf-8")
        environment = {key: value for key, value in os.environ.items() if key != "MARMOT_PATH"}
        if IS_WINDOWS:
            environment["MARMOT_PATH"] = str(root / "MarmotPrelude")
            environment["PATH"] = str(root / "bin") + os.pathsep + environment.get("PATH", "")
            command = "marmot --version && marmotc build Hello.mmt -o Hello.mmc && marmotvm run Hello.mmc"
            completed = subprocess.run(command, shell=True, cwd=root, env=environment, capture_output=True, text=True)
        else:
            command = "./install.sh > /dev/null && . ./env.sh && marmot --version && marmotc build Hello.mmt -o Hello.mmc && marmotvm run Hello.mmc"
            completed = subprocess.run(["sh", "-c", command], cwd=root, env=environment, capture_output=True, text=True)
    if completed.returncode != 0 or "hello from the package" not in completed.stdout:
        console.fail(f"the extracted package did not run a program:\n{completed.stdout}{completed.stderr}")
        return False
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Package this checkout's build for this platform.")
    add_build_arguments(parser, default="Release")
    args = parser.parse_args(argv)

    built = cargo.build(release=True)
    if built != 0:
        return built

    name = f"marmot-{'windows' if IS_WINDOWS else 'linux'}-x64"
    target = InstallLayout(PACKAGE_DIR / name)
    stage(BuildTree.from_args(args), target)
    archive = Path(shutil.make_archive(str(target.root), "zip" if IS_WINDOWS else "gztar", PACKAGE_DIR, name))
    if not smoke_test(archive, name):
        archive.unlink()
        return 1

    console.ok(f"packaged {archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
