#!/usr/bin/env python3
"""
Marmot uninstaller.

Removes the install's MarmotPrelude entry from MARMOT_PATH and removes the install's bin directory
from PATH. Optionally deletes the installed files.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass(frozen=True)
class InstallLayout:
    root: Path
    prelude_dir: Path
    bin_dir: Path


INSTALL_MARKER_FILENAME = "marmot_install.json"


def is_windows() -> bool:
    return os.name == "nt"


def default_install_root(scope: str) -> Path:
    if scope == "machine":
        program_files = os.environ.get("ProgramFiles", "")
        if program_files == "":
            raise RuntimeError("ProgramFiles is not set.")
        return Path(program_files) / "Marmot"

    if not is_windows():
        data_home = os.environ.get("XDG_DATA_HOME", "")
        return (Path(data_home) if data_home != "" else Path.home() / ".local" / "share") / "marmot"

    local_appdata = os.environ.get("LOCALAPPDATA", "")
    if local_appdata == "":
        raise RuntimeError("LOCALAPPDATA is not set.")
    return Path(local_appdata) / "Marmot"


def layout(scope: str, install_dir: Optional[str]) -> InstallLayout:
    root = Path(install_dir).expanduser().resolve() if install_dir else default_install_root(scope).resolve()
    return InstallLayout(
        root=root,
        prelude_dir=root / "MarmotPrelude",
        bin_dir=root / "bin",
    )


def strip_surrounding_quotes(value: str) -> str:
    if len(value) < 2:
        return value

    if (value.startswith('"') and value.endswith('"')) or (value.startswith("'") and value.endswith("'")):
        return value[1:-1]
    return value


def normalize_env_path(path_value: str) -> str:
    unquoted = strip_surrounding_quotes(path_value.strip())
    return os.path.normcase(os.path.normpath(unquoted))


def split_path_list(value: str) -> list[str]:
    if value.strip() == "":
        return []
    return [item.strip() for item in value.split(os.pathsep) if item.strip() != ""]


def remove_path_entries(existing: str, to_remove: Path) -> str:
    candidate = str(to_remove.resolve())
    entries = split_path_list(existing)
    normalized_candidate = normalize_env_path(candidate)
    kept = [entry for entry in entries if normalize_env_path(entry) != normalized_candidate]
    return os.pathsep.join(kept)


def require_admin_for_machine(scope: str) -> None:
    if scope != "machine":
        return

    if not is_windows():
        raise RuntimeError("Machine scope is only supported on Windows.")

    try:
        import ctypes

        is_admin = ctypes.windll.shell32.IsUserAnAdmin() != 0
    except Exception as exc:
        raise RuntimeError("Unable to determine administrator privileges.") from exc

    if not is_admin:
        raise RuntimeError("Scope 'machine' requires running the uninstaller as Administrator.")


def broadcast_environment_change_windows() -> None:
    import ctypes
    from ctypes import wintypes

    HWND_BROADCAST = 0xFFFF
    WM_SETTINGCHANGE = 0x001A
    SMTO_ABORTIFHUNG = 0x0002

    SendMessageTimeoutW = ctypes.windll.user32.SendMessageTimeoutW
    SendMessageTimeoutW.argtypes = [
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPCWSTR,
        wintypes.UINT,
        wintypes.UINT,
        ctypes.POINTER(wintypes.DWORD),
    ]
    SendMessageTimeoutW.restype = wintypes.LPARAM

    result = wintypes.DWORD(0)
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment", SMTO_ABORTIFHUNG, 5000, ctypes.byref(result))


def read_windows_env(scope: str, name: str) -> tuple[str, int, bool]:
    import winreg

    if scope == "machine":
        root = winreg.HKEY_LOCAL_MACHINE
        subkey = r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
    else:
        root = winreg.HKEY_CURRENT_USER
        subkey = r"Environment"

    with winreg.OpenKey(root, subkey, 0, winreg.KEY_READ) as key:
        try:
            value, value_type = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return "", winreg.REG_EXPAND_SZ, False

    if not isinstance(value, str):
        return "", value_type, True
    return value, value_type, True


def write_windows_env(scope: str, name: str, value: str, value_type: int) -> None:
    import winreg

    if scope == "machine":
        root = winreg.HKEY_LOCAL_MACHINE
        subkey = r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
    else:
        root = winreg.HKEY_CURRENT_USER
        subkey = r"Environment"

    access = winreg.KEY_SET_VALUE
    with winreg.OpenKey(root, subkey, 0, access) as key:
        winreg.SetValueEx(key, name, 0, value_type, value)


def delete_windows_env(scope: str, name: str) -> None:
    import winreg

    if scope == "machine":
        root = winreg.HKEY_LOCAL_MACHINE
        subkey = r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
    else:
        root = winreg.HKEY_CURRENT_USER
        subkey = r"Environment"

    access = winreg.KEY_SET_VALUE
    with winreg.OpenKey(root, subkey, 0, access) as key:
        try:
            winreg.DeleteValue(key, name)
        except FileNotFoundError:
            return


def safe_remove_install_dir(target_layout: InstallLayout, scope: str, force: bool) -> None:
    if not target_layout.root.exists():
        return

    resolved_root = target_layout.root.resolve()
    resolved_anchor = Path(resolved_root.anchor).resolve()
    if resolved_root == resolved_anchor:
        raise RuntimeError(f"Refusing to remove filesystem root: {resolved_root}")

    marker_path = target_layout.root / INSTALL_MARKER_FILENAME
    has_marker = marker_path.is_file()
    default_root = default_install_root(scope).resolve()
    has_expected_files = target_layout.prelude_dir.is_dir() or (target_layout.bin_dir / ("marmotc.exe" if is_windows() else "marmotc")).is_file()

    if not force and not (has_marker or (resolved_root == default_root and has_expected_files)):
        raise RuntimeError(
            f"Refusing to remove '{resolved_root}' because it does not look like a Marmot installation. "
            f"Expected '{INSTALL_MARKER_FILENAME}' or a default install layout. Pass --force to override."
        )

    shutil.rmtree(resolved_root)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Uninstall Marmot and remove MARMOT_PATH / PATH entries.")
    parser.add_argument("--scope", choices=["user", "machine"], default="user", help="Target environment scope (default: user).")
    parser.add_argument("--install-dir", default="", help="Installation directory (defaults to LocalAppData/ProgramFiles based on scope, or ~/.local/share/marmot off Windows).")
    parser.add_argument("--keep-files", action="store_true", help="Do not remove installed files; only update env vars.")
    parser.add_argument("--force", action="store_true", help="Remove install directory even if it does not look like Marmot.")
    args = parser.parse_args(argv)

    require_admin_for_machine(args.scope)

    target_layout = layout(args.scope, args.install_dir if args.install_dir != "" else None)

    if is_windows():
        marmot_path_value, marmot_path_type, marmot_path_exists = read_windows_env(args.scope, "MARMOT_PATH")
        updated_midori_path = remove_path_entries(marmot_path_value, target_layout.prelude_dir)
        if updated_midori_path.strip() == "":
            if marmot_path_exists:
                delete_windows_env(args.scope, "MARMOT_PATH")
            os.environ.pop("MARMOT_PATH", None)
        else:
            write_windows_env(args.scope, "MARMOT_PATH", updated_midori_path, marmot_path_type)
            os.environ["MARMOT_PATH"] = updated_midori_path

        path_value, path_type, path_exists = read_windows_env(args.scope, "PATH")
        updated_path = remove_path_entries(path_value, target_layout.bin_dir)
        if updated_path.strip() == "":
            if path_exists:
                delete_windows_env(args.scope, "PATH")
            os.environ.pop("PATH", None)
        else:
            write_windows_env(args.scope, "PATH", updated_path, path_type)
            os.environ["PATH"] = updated_path

        broadcast_environment_change_windows()
    else:
        print("Non-Windows uninstall: did not update persistent environment variables.", file=sys.stderr)

    if not args.keep_files:
        safe_remove_install_dir(target_layout, args.scope, args.force)

    print(f"Removed MARMOT_PATH entry: {target_layout.prelude_dir}")
    print(f"Removed PATH entry: {target_layout.bin_dir}")
    if args.keep_files:
        print(f"Kept install directory: {target_layout.root}")
    else:
        print(f"Removed install directory: {target_layout.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
