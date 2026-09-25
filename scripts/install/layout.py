"""Where Marmot installs, and the environment variables that point at it.

Windows keeps MARMOT_PATH and PATH in the registry, for the user or the
machine. Elsewhere the install writes env.sh, for the shell profile to source.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from lib.host import IS_WINDOWS

MARKER_FILENAME = "marmot_install.json"
ENV_SCRIPT_FILENAME = "env.sh"
_REGISTRY_KEYS = {"user": "Environment", "machine": r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"}


@dataclass(frozen=True)
class InstallLayout:
    root: Path

    @staticmethod
    def resolve(scope: str, install_dir: str | None) -> "InstallLayout":
        return InstallLayout(Path(install_dir).expanduser().resolve() if install_dir else default_root(scope).resolve())

    @property
    def prelude_dir(self) -> Path:
        return self.root / "MarmotPrelude"

    @property
    def bin_dir(self) -> Path:
        return self.root / "bin"

    @property
    def marker(self) -> Path:
        return self.root / MARKER_FILENAME

    @property
    def env_script(self) -> Path:
        return self.root / ENV_SCRIPT_FILENAME


def default_root(scope: str) -> Path:
    if scope == "machine":
        return Path(os.environ["ProgramFiles"]) / "Marmot"
    if IS_WINDOWS:
        return Path(os.environ["LOCALAPPDATA"]) / "Marmot"
    data_home = os.environ.get("XDG_DATA_HOME", "")
    return (Path(data_home) if data_home else Path.home() / ".local" / "share") / "marmot"


def require_scope_allowed(scope: str) -> None:
    if scope != "machine":
        return
    if not IS_WINDOWS:
        raise SystemExit("--scope machine is for Windows; elsewhere pass --install-dir, e.g. /opt/marmot.")
    import ctypes

    if ctypes.windll.shell32.IsUserAnAdmin() == 0:
        raise SystemExit("--scope machine needs an Administrator prompt.")


def _normalized(entry: str) -> str:
    unquoted = entry.strip().strip('"').strip("'")
    return os.path.normcase(os.path.normpath(unquoted))


def split_path_list(value: str) -> list[str]:
    return [entry.strip() for entry in value.split(os.pathsep) if entry.strip()]


def with_entry(existing: str, entry: Path) -> str:
    entries = split_path_list(existing)
    if _normalized(str(entry)) in map(_normalized, entries):
        return os.pathsep.join(entries)
    return os.pathsep.join([*entries, str(entry)])


def without_entry(existing: str, entry: Path) -> str:
    return os.pathsep.join(item for item in split_path_list(existing) if _normalized(item) != _normalized(str(entry)))


def read_registry(scope: str, name: str) -> tuple[str, int]:
    import winreg

    hive = winreg.HKEY_LOCAL_MACHINE if scope == "machine" else winreg.HKEY_CURRENT_USER
    with winreg.OpenKey(hive, _REGISTRY_KEYS[scope], 0, winreg.KEY_READ) as key:
        try:
            value, value_type = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return "", winreg.REG_EXPAND_SZ
    return (value if isinstance(value, str) else ""), value_type


def write_registry(scope: str, name: str, value: str, value_type: int) -> None:
    import winreg

    hive = winreg.HKEY_LOCAL_MACHINE if scope == "machine" else winreg.HKEY_CURRENT_USER
    with winreg.OpenKey(hive, _REGISTRY_KEYS[scope], 0, winreg.KEY_SET_VALUE) as key:
        if value:
            winreg.SetValueEx(key, name, 0, value_type, value)
            return
        try:
            winreg.DeleteValue(key, name)
        except FileNotFoundError:
            pass


def update_registry_list(scope: str, name: str, change) -> None:
    """Rewrite a registry path list with `change(old) -> new`; an empty list deletes the variable."""
    value, value_type = read_registry(scope, name)
    write_registry(scope, name, change(value), value_type)


def broadcast_environment_change() -> None:
    """Tells running programs, Explorer among them, that the environment changed."""
    import ctypes
    from ctypes import wintypes

    send = ctypes.windll.user32.SendMessageTimeoutW
    send.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPCWSTR, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    send.restype = wintypes.LPARAM
    # lpdwResult is a PDWORD_PTR: pointer-sized, so a DWORD would be overrun on x64.
    result = ctypes.c_size_t(0)
    hwnd_broadcast, wm_settingchange, smto_abortifhung = 0xFFFF, 0x001A, 0x0002
    send(hwnd_broadcast, wm_settingchange, 0, "Environment", smto_abortifhung, 5000, ctypes.byref(result))
