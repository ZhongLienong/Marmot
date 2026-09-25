"""Terminal output: colours when the terminal shows them, and echoed commands."""

from __future__ import annotations

import os
import shlex
import subprocess
import sys

from lib.host import IS_WINDOWS


def _enable_colour() -> bool:
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return False
    if IS_WINDOWS:
        # Asks the Windows console to interpret ANSI sequences.
        os.system("")
    return True


class Color:
    _on = _enable_colour()
    RED = "\033[91m" if _on else ""
    GREEN = "\033[92m" if _on else ""
    YELLOW = "\033[93m" if _on else ""
    BLUE = "\033[94m" if _on else ""
    CYAN = "\033[96m" if _on else ""
    GRAY = "\033[90m" if _on else ""
    BOLD = "\033[1m" if _on else ""
    RESET = "\033[0m" if _on else ""


def format_command(command: list[str]) -> str:
    return subprocess.list2cmdline(command) if IS_WINDOWS else shlex.join(command)


def announce(command: list[str]) -> None:
    print(f"\n{Color.GRAY}> {format_command(command)}{Color.RESET}", flush=True)


def ok(message: str) -> None:
    print(f"{Color.GREEN}[OK]{Color.RESET} {message}")


def fail(message: str) -> None:
    print(f"{Color.RED}[FAIL]{Color.RESET} {message}")


def note(message: str) -> None:
    print(f"{Color.YELLOW}{message}{Color.RESET}")
