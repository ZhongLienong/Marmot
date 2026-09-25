"""The C++ toolchain: the environment a build runs in, and running commands in it.

On Windows the compiler is MSVC, whose environment a Developer Prompt sets up;
it is loaded here from vcvars64.bat, found with vswhere, so any shell will do.
On Linux the default compiler may predate C++23's library (GCC 13 has no
<print>), so a fresh configure picks one that has it unless CXX says otherwise.
"""

from __future__ import annotations

import functools
import os
import re
import shutil
import subprocess
from pathlib import Path

from lib import console
from lib.host import IS_WINDOWS, REPO_ROOT

# GCC 14 is the first with <print>; Clang 18 cannot use libstdc++'s std::expected.
_MINIMUM_GCC = 14
_MINIMUM_CLANG = 19
_LINUX_CANDIDATES = ["g++-16", "g++-15", "g++-14", "clang++-22", "clang++-21", "clang++-20", "clang++-19", "g++", "clang++", "c++"]


def _vswhere() -> Path:
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    return Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def visual_studio_root() -> Path | None:
    vswhere = _vswhere()
    if not vswhere.is_file():
        return None
    found = subprocess.run(
        [str(vswhere), "-latest", "-prerelease", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=False,
    )
    lines = found.stdout.strip().splitlines()
    return Path(lines[0]) if lines else None


@functools.cache
def msvc_environment() -> dict[str, str]:
    """os.environ with vcvars64.bat applied, or unchanged inside a Developer Prompt."""
    if shutil.which("cl") is not None:
        return os.environ.copy()

    root = visual_studio_root()
    if root is None:
        raise SystemExit("Visual Studio with the C++ x64 tools was not found (vswhere found no installation). "
                         "Install the 'Desktop development with C++' workload, or run from a Developer Prompt.")
    vcvars = root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    # A string, not a list: cmd /s strips the outer quotes and keeps the ones
    # around the path, which list quoting would escape.
    dumped = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, check=False)
    if dumped.returncode != 0:
        raise SystemExit(f"{vcvars} failed:\n{dumped.stdout}{dumped.stderr}")

    environment: dict[str, str] = {}
    for line in dumped.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            environment[key] = value
    return environment


def _compiler_version(compiler: str) -> tuple[str, int] | None:
    described = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=False)
    if described.returncode != 0:
        return None
    first_line = described.stdout.splitlines()[0] if described.stdout else ""
    family = "clang" if "clang" in first_line.lower() else "gcc"
    match = re.search(r"(\d+)\.\d+(\.\d+)?", first_line)
    return (family, int(match.group(1))) if match else None


def supports_cxx23_library(compiler: str) -> bool:
    version = _compiler_version(compiler)
    if version is None:
        return False
    family, major = version
    return major >= (_MINIMUM_CLANG if family == "clang" else _MINIMUM_GCC)


@functools.cache
def linux_cxx() -> str | None:
    """A C++ compiler new enough for the sources, when CXX does not name one."""
    if os.environ.get("CXX"):
        return None
    for candidate in _LINUX_CANDIDATES:
        path = shutil.which(candidate)
        if path is not None and supports_cxx23_library(path):
            return path
    return None


def build_environment(configuring: bool = False) -> dict[str, str]:
    """The environment to run CMake, Ninja and the compiler in.

    A build tree remembers its compiler, so the Linux choice only matters to a
    configure that creates one.
    """
    if IS_WINDOWS:
        return msvc_environment()

    environment = os.environ.copy()
    chosen = linux_cxx() if configuring else None
    if chosen is not None:
        environment["CXX"] = chosen
    return environment


def find_program(name: str, environment: dict[str, str]) -> str | None:
    search_path = next((value for key, value in environment.items() if key.upper() == "PATH"), None)
    return shutil.which(name, path=search_path)


def find_build_tool(name: str) -> str | None:
    """cmake, ninja or ctest: on PATH, or on Windows the ones Visual Studio ships."""
    found = shutil.which(name)
    if found is None and IS_WINDOWS and visual_studio_root() is not None:
        return find_program(name, msvc_environment())
    return found


def resolve_program(command: list[str], environment: dict[str, str] | None) -> list[str]:
    """The command with its program found on `environment`'s PATH.

    Windows looks a program up on the parent's PATH, not on the PATH of the
    environment it is started with, and cmake and ninja may be on vcvars' only.
    """
    if environment is None:
        return command
    found = find_program(command[0], environment)
    return [found, *command[1:]] if found is not None else command


def run(command: list[str], *, environment: dict[str, str] | None = None, cwd: Path = REPO_ROOT) -> int:
    console.announce(command)
    return subprocess.run(resolve_program(command, environment), cwd=cwd, env=environment, check=False).returncode
