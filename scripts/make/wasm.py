#!/usr/bin/env python3
"""
Build Marmot for WebAssembly with Emscripten, and optionally deploy it to a website.

The build goes to build-wasm/ and makes marmot.js and marmot.wasm. Deploying
copies them, the prelude and a manifest of its files into the site's public
folder, given with --deploy or the MARMOT_SITE_DIR environment variable.

Emscripten is found on PATH, else in $EMSDK or a usual emsdk folder.

Examples:
    python scripts/dev.py wasm
    python scripts/dev.py wasm --clean
    python scripts/dev.py wasm --deploy ../ZhongLienong.github.io/public
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib import console, toolchain
from lib.host import IS_WINDOWS, PRELUDE_DIR, REPO_ROOT

BUILD_DIR = REPO_ROOT / "build-wasm"
OUTPUT_DIR = BUILD_DIR / "out"
WASM_FILES = ["marmot.js", "marmot.wasm"]


@dataclass(frozen=True)
class Emscripten:
    environment: dict[str, str]
    emcmake: str
    root: Path | None = None


def format_size(size_bytes: int) -> str:
    size_kb = size_bytes / 1024
    return f"{size_kb / 1024:.2f} MB" if size_kb >= 1024 else f"{size_kb:.2f} KB"


def newest_directory(directory: Path) -> Path | None:
    if not directory.is_dir():
        return None
    return max((path for path in directory.iterdir() if path.is_dir()), default=None)


def from_emsdk(emsdk_root: Path) -> Emscripten | None:
    emscripten_root = emsdk_root / "upstream" / "emscripten"
    emcmake = emscripten_root / ("emcmake.bat" if IS_WINDOWS else "emcmake")
    if not emcmake.is_file():
        return None

    environment = os.environ.copy()
    environment["EMSDK"] = emsdk_root.as_posix()
    environment["EM_CONFIG"] = str(emsdk_root / ".emscripten")
    environment["EMSDK_QUIET"] = "1"
    node_dir = newest_directory(emsdk_root / "node")
    if node_dir is not None:
        environment["EMSDK_NODE"] = str(node_dir / "bin" / ("node.exe" if IS_WINDOWS else "node"))
    python_dir = newest_directory(emsdk_root / "python")
    if python_dir is not None:
        environment["EMSDK_PYTHON"] = str(python_dir / ("python.exe" if IS_WINDOWS else "python3"))
    environment["PATH"] = os.pathsep.join([str(emsdk_root), str(emscripten_root), environment.get("PATH", "")])
    return Emscripten(environment, str(emcmake), emsdk_root)


def find_emscripten() -> Emscripten | None:
    on_path = shutil.which("emcmake")
    if on_path is not None:
        return Emscripten(os.environ.copy(), on_path)

    usual = [Path("C:/Program Files/emsdk-main"), Path("C:/emsdk-main"), Path("C:/emsdk")] if IS_WINDOWS else [Path("/opt/emsdk")]
    roots = [Path(os.environ["EMSDK"])] if os.environ.get("EMSDK") else []
    roots += [Path.home() / "emsdk", *usual]
    return next((found for found in map(from_emsdk, roots) if found is not None), None)


def remove_locked(function, path, _error) -> None:
    # Windows: read-only files, and files an indexer or antivirus holds briefly.
    os.chmod(path, stat.S_IWRITE)
    for _ in range(30):
        try:
            function(path)
            return
        except PermissionError:
            time.sleep(0.5)
    console.note(f"skipping locked path: {path}")


def build(emscripten: Emscripten, clean: bool) -> int:
    if clean and BUILD_DIR.exists():
        print(f"Cleaning {BUILD_DIR}...")
        # onerror became onexc in 3.12; the handler ignores the argument that changed.
        handler = {"onexc" if sys.version_info >= (3, 12) else "onerror": remove_locked}
        shutil.rmtree(BUILD_DIR, **handler)

    if not (BUILD_DIR / "CMakeCache.txt").is_file():
        generator = ["-G", "Ninja"] if shutil.which("ninja", path=emscripten.environment.get("PATH")) else []
        configured = toolchain.run(
            [emscripten.emcmake, "cmake", "-S", str(REPO_ROOT), "-B", str(BUILD_DIR), *generator,
             "-DCMAKE_BUILD_TYPE=Release", "-DMIDORI_WASM64=ON"],
            environment=emscripten.environment,
        )
        if configured != 0:
            return configured
    return toolchain.run(["cmake", "--build", str(BUILD_DIR), "--config", "Release", "--parallel"], environment=emscripten.environment)


def deploy(site: Path) -> int:
    if not site.is_dir():
        console.fail(f"No website folder at {site}")
        return 1

    print(f"\nDeploying to {site}...")
    for filename in WASM_FILES:
        source = OUTPUT_DIR / filename
        shutil.copy2(source, site / filename)
        print(f"  {filename} ({format_size(source.stat().st_size)})")

    prelude = site / "MarmotPrelude"
    if prelude.exists():
        shutil.rmtree(prelude)
    shutil.copytree(PRELUDE_DIR, prelude)
    manifest = sorted(path.relative_to(prelude).as_posix() for path in prelude.rglob("*.mmt"))
    (prelude / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"  MarmotPrelude/ ({len(manifest)} files)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build Marmot for WebAssembly, and optionally deploy it.")
    parser.add_argument("--clean", action="store_true", help="Delete build-wasm/ first.")
    parser.add_argument("--deploy", nargs="?", const=os.environ.get("MARMOT_SITE_DIR", ""), default=None, metavar="SITE_DIR",
                        help="Copy the build and the prelude into this website folder (default: $MARMOT_SITE_DIR).")
    args = parser.parse_args(argv)
    if args.deploy == "":
        parser.error("--deploy needs a folder, or MARMOT_SITE_DIR set")

    emscripten = find_emscripten()
    if emscripten is None:
        console.fail("Emscripten not found. Activate it first (emsdk_env.bat on Windows, `source emsdk_env.sh` elsewhere), or set EMSDK.")
        return 1
    if emscripten.root is not None:
        print(f"Using Emscripten from {emscripten.root}")

    built = build(emscripten, args.clean)
    if built != 0:
        return built
    missing = [name for name in WASM_FILES if not (OUTPUT_DIR / name).is_file()]
    if missing:
        console.fail(f"The build made no {', '.join(missing)} in {OUTPUT_DIR}")
        return 1
    console.ok(f"built {', '.join(WASM_FILES)} in {OUTPUT_DIR}")

    if args.deploy is not None:
        return deploy(Path(args.deploy).expanduser().resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
