"""The repository and the host it is running on."""

from __future__ import annotations

import contextlib
import os
import shutil
import sys
from collections.abc import Iterable, Iterator
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
PRELUDE_DIR = REPO_ROOT / "MarmotPrelude"
TEST_DIR = REPO_ROOT / "test"
TOOL_DIR = REPO_ROOT / "projects" / "marmot"

IS_WINDOWS = os.name == "nt"
IS_LINUX = sys.platform.startswith("linux")


def executable(stem: str) -> str:
    return stem + (".exe" if IS_WINDOWS else "")


def preset_family() -> str:
    """The CMakePresets.json presets for this host: x64-* on Windows, linux-* on Linux."""
    if IS_WINDOWS:
        return "x64-"
    if IS_LINUX:
        return "linux-"
    raise SystemExit(f"CMakePresets.json has presets for Windows and Linux only, not {sys.platform}.")


def prelude_search_path(existing: str | None = None) -> str:
    """MARMOT_PATH with this checkout's prelude first: an installed one may be older."""
    entries = [str(PRELUDE_DIR)]
    current = os.environ.get("MARMOT_PATH", "") if existing is None else existing
    if current:
        entries.append(current)
    return os.pathsep.join(entries)


def checkout_environment(**overrides: str | None) -> dict[str, str]:
    """The environment a Marmot program built from this checkout should see."""
    environment = os.environ.copy()
    environment["MARMOT_PATH"] = prelude_search_path()
    for key, value in overrides.items():
        if value is None:
            environment.pop(key, None)
        else:
            environment[key] = value
    return environment


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


# What building and running programs from the checkout leaves beside them.
BUILD_ARTIFACTS = ("*.ppm", "*.mmc", "*.mmc.json")


def remove_artifacts(directory: Path, patterns: Iterable[str] = BUILD_ARTIFACTS, extra: Iterable[Path] = ()) -> None:
    """Delete what matches `patterns` in `directory`, and `extra`, files or folders."""
    for path in [*(match for pattern in patterns for match in directory.glob(pattern)), *extra]:
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            with contextlib.suppress(OSError):
                path.unlink()


@contextlib.contextmanager
def removing_new_files(directory: Path) -> Iterator[None]:
    """Delete the files that appear directly in `directory` while the block runs."""
    before = {path for path in directory.iterdir() if path.is_file()}
    try:
        yield
    finally:
        remove_artifacts(directory, (), {path for path in directory.iterdir() if path.is_file()} - before)
