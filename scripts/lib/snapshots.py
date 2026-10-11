"""What a test's snapshots compare: its output without colour or this checkout's
path, and the compiler's machine-readable warnings as JSON."""

from __future__ import annotations

import json
import re
from pathlib import Path

from lib.host import REPO_ROOT

WARNING_PREFIX = "MARMOT_WARNING\t"


def without_checkout(text: str) -> str:
    clean = text.replace("\r\n", "\n").replace("\r", "")
    root = str(REPO_ROOT.resolve())
    for prefix in {root, root.replace("\\", "/")}:
        clean = clean.replace(prefix + "\\", "").replace(prefix + "/", "")
    return clean.replace("\\", "/")


def snapshot_text(text: str) -> str:
    clean = without_checkout(re.sub(r"\x1b\[[0-9;]*m", "", text))
    clean = re.sub(r"(^\d+ \| .*)\n+(?=\s+\|)", r"\1\n", clean, flags=re.MULTILINE)
    return "\n".join(line.rstrip() for line in clean.split("\n"))


def normalized_warning(warning: dict) -> dict:
    return {key: without_checkout(value) if key in ("file", "file_path") and isinstance(value, str) else value
            for key, value in warning.items()}


def split_warnings(output: str) -> tuple[list[dict], str]:
    """The warnings MARMOT_TEST_WARNING_FORMAT=machine adds to the output, and the rest of it."""
    warnings: list[dict] = []
    rest: list[str] = []
    for line in output.splitlines():
        if line.startswith(WARNING_PREFIX):
            warnings.append(normalized_warning(json.loads(line.removeprefix(WARNING_PREFIX))))
        else:
            rest.append(line)
    return warnings, "\n".join(rest)


def read_warnings(path: Path) -> list[dict] | None:
    """A .warnings.json snapshot, or None when there is none."""
    if not path.exists():
        return None
    return [normalized_warning(warning) for warning in json.loads(path.read_text(encoding="utf-8"))]
