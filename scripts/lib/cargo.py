"""The Rust `marmot` project tool under tool/, built and tested with cargo."""

from __future__ import annotations

import shutil
from pathlib import Path

from lib import toolchain
from lib.host import TOOL_DIR, checkout_environment, executable

MANIFEST = TOOL_DIR / "Cargo.toml"


def is_available() -> bool:
    return shutil.which("cargo") is not None


def binary(profile: str) -> Path:
    return TOOL_DIR / "target" / profile / executable("marmot")


def newest_binary() -> Path | None:
    built = [path for path in (binary("release"), binary("debug")) if path.is_file()]
    return max(built, key=lambda path: path.stat().st_mtime, default=None)


def is_stale(tool: Path) -> bool:
    newest_source = max((source.stat().st_mtime for source in (TOOL_DIR / "src").rglob("*.rs")), default=0.0)
    return tool.stat().st_mtime < newest_source


def build(release: bool = True) -> int:
    return toolchain.run(["cargo", "build", "--manifest-path", str(MANIFEST), *(["--release"] if release else [])])


def test(compiler: Path) -> int:
    """The tool's unit tests, plus its end-to-end runs against `compiler`."""
    return toolchain.run(["cargo", "test", "--manifest-path", str(MANIFEST)], environment=checkout_environment(MARMOTC=str(compiler)))
