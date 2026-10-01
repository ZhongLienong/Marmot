#!/usr/bin/env python3
"""
Build Marmot for WebAssembly with Emscripten, and optionally deploy it to a website.

The independent builds go to out/build/<project>/wasm64/. Deploying copies
marmotc.js/.wasm, marmotvm.js/.wasm, the marmot.js adapter, and the prelude into the site's public
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

PROJECTS = ("marmotc", "marmotvm")
ADAPTER = REPO_ROOT / "projects" / "web" / "marmot.js"


def build_directory(project: str) -> Path:
    return REPO_ROOT / "out" / "build" / project / "wasm64"


def artifacts() -> dict[str, Path]:
    return {"marmot.js": ADAPTER, **{
        f"{project}.{suffix}": build_directory(project) / "out" / f"{project}.{suffix}"
        for project in PROJECTS for suffix in ("js", "wasm")
    }}


@dataclass(frozen=True)
class Emscripten:
    environment: dict[str, str]
    emcmake: str
    root: Path | None = None

    @property
    def toolchain_file(self) -> Path:
        return Path(self.emcmake).resolve().parent / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"


def format_size(size_bytes: int) -> str:
    size_kb = size_bytes / 1024
    return f"{size_kb / 1024:.2f} MB" if size_kb >= 1024 else f"{size_kb:.2f} KB"


def newest_directory(directory: Path) -> Path | None:
    if not directory.is_dir():
        return None
    return max((path for path in directory.iterdir() if path.is_dir()), default=None)


def from_emsdk(emsdk_root: Path) -> Emscripten | None:
    emscripten_root = emsdk_root / "upstream" / "emscripten"
    # Newer emsdk releases ship emcmake.exe on Windows in place of emcmake.bat.
    launchers = ["emcmake.exe", "emcmake.bat"] if IS_WINDOWS else ["emcmake"]
    emcmake = next((emscripten_root / name for name in launchers if (emscripten_root / name).is_file()), None)
    if emcmake is None:
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


def cached_path(directory: Path, variable: str) -> Path | None:
    cache = directory / "CMakeCache.txt"
    if not cache.is_file():
        return None
    prefix = variable + "="
    lines = cache.read_text(encoding="utf-8", errors="replace").splitlines()
    return next((Path(line[len(prefix):]).resolve() for line in lines if line.startswith(prefix)), None)


def build_project(emscripten: Emscripten, project: str, clean: bool) -> int:
    directory = build_directory(project)
    if not directory.resolve().is_relative_to((REPO_ROOT / "out" / "build").resolve()):
        raise SystemExit(f"Build output must stay inside out/build: {directory}")
    if clean and directory.exists():
        print(f"Cleaning {directory}...")
        # onerror became onexc in 3.12; the handler ignores the argument that changed.
        handler = {"onexc" if sys.version_info >= (3, 12) else "onerror": remove_locked}
        shutil.rmtree(directory, **handler)

    # A build tree keeps the em++ it was configured with; running it under another emsdk's config mixes LLVM versions.
    cached = cached_path(directory, "CMAKE_TOOLCHAIN_FILE:FILEPATH")
    source = REPO_ROOT / "projects" / project
    stale = cached is not None and (cached != emscripten.toolchain_file.resolve() or cached_path(directory, "CMAKE_HOME_DIRECTORY:INTERNAL") != source.resolve())
    if stale:
        console.note(f"{directory} has a different toolchain or source directory; reconfiguring")
    if cached is None or stale:
        generator = ["-G", "Ninja"] if shutil.which("ninja", path=emscripten.environment.get("PATH")) else []
        fresh = ["--fresh"] if stale else []
        configured = toolchain.run(
            [emscripten.emcmake, "cmake", *fresh, "-S", str(source), "-B", str(directory), *generator,
             "-DCMAKE_BUILD_TYPE=Release", "-DMIDORI_BUILD_TESTS=OFF", "-DMIDORI_WASM64=ON"],
            environment=emscripten.environment,
        )
        if configured != 0:
            return configured
    return toolchain.run(["cmake", "--build", str(directory), "--config", "Release", "--target", project, "--parallel"], environment=emscripten.environment)


def build(emscripten: Emscripten, clean: bool) -> int:
    for project in PROJECTS:
        status = build_project(emscripten, project, clean)
        if status != 0:
            return status
    return 0


def deploy(site: Path) -> int:
    if not site.is_dir():
        console.fail(f"No website folder at {site}")
        return 1

    print(f"\nDeploying to {site}...")
    for filename, source in artifacts().items():
        shutil.copy2(source, site / filename)
        print(f"  {filename} ({format_size(source.stat().st_size)})")

    (site / "marmot.wasm").unlink(missing_ok=True)

    prelude = site / "MarmotPrelude"
    if not prelude.resolve().is_relative_to(site.resolve()):
        raise SystemExit(f"Prelude output must stay inside the website folder: {prelude}")
    if prelude.exists():
        shutil.rmtree(prelude)
    shutil.copytree(PRELUDE_DIR, prelude)
    manifest = sorted(path.relative_to(prelude).as_posix() for path in prelude.rglob("*.mmt"))
    (prelude / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"  MarmotPrelude/ ({len(manifest)} files)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build Marmot for WebAssembly, and optionally deploy it.")
    parser.add_argument("--clean", action="store_true", help="Delete the WebAssembly build tree first.")
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
    missing = [name for name, path in artifacts().items() if not path.is_file()]
    if missing:
        console.fail(f"The build made no {', '.join(missing)}")
        return 1
    console.ok(f"built independent compiler and VM modules: {', '.join(artifacts())}")

    if args.deploy is not None:
        return deploy(Path(args.deploy).expanduser().resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
