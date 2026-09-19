#!/usr/bin/env python3
"""
Marmot installer.

Installs MarmotPrelude to a local directory and configures MARMOT_PATH so system imports
like `import { <IO> }` can be resolved.

Optionally installs the compiler (marmotc), the VM that runs what it builds
(marmotvm, from beside marmotc) and the project tool (marmot), and updates PATH
so they are callable from a new cmd/PowerShell session.

Note: builtin FFI functions are statically linked into marmotvm.exe, so no
separate DLL is needed.
"""

from __future__ import annotations

import argparse
import json
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


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def default_install_root(scope: str) -> Path:
    if scope == "machine":
        program_files = os.environ.get("ProgramFiles", "")
        if program_files == "":
            raise RuntimeError("ProgramFiles is not set.")
        return Path(program_files) / "Marmot"

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


def copy_dir_contents(src_dir: Path, dst_dir: Path) -> None:
    if dst_dir.exists():
        if dst_dir.is_symlink() or dst_dir.is_file():
            dst_dir.unlink()
        else:
            shutil.rmtree(dst_dir)

    shutil.copytree(src_dir, dst_dir)


def normalize_env_path(path_value: str) -> str:
    return os.path.normcase(os.path.normpath(path_value))


def split_path_list(value: str) -> list[str]:
    if value.strip() == "":
        return []
    return [item.strip() for item in value.split(os.pathsep) if item.strip() != ""]


def append_unique_path(existing: str, to_append: Path) -> str:
    candidate = str(to_append.resolve())
    entries = split_path_list(existing)
    normalized = [normalize_env_path(item) for item in entries]

    if normalize_env_path(candidate) in normalized:
        return os.pathsep.join(entries)

    return os.pathsep.join(entries + [candidate])


def get_preset_executable_candidates(repo: Path, preset_name: str) -> list[Path]:
    return [
        repo / "out" / "build" / "ninja" / preset_name / "out" / "marmotc.exe",
        repo / "out" / "build" / preset_name / "out" / "marmotc.exe",
    ]


def resolve_midori_exe(repo: Path, preset: str, explicit_exe: Optional[str]) -> Optional[Path]:
    if explicit_exe:
        exe_path = Path(explicit_exe).expanduser().resolve()
        return exe_path if exe_path.is_file() else None

    presets: list[str]
    if preset == "auto":
        presets = ["x64-release", "x64-development", "x64-debug"]
    else:
        presets = [preset]

    for preset_name in presets:
        for candidate in get_preset_executable_candidates(repo, preset_name):
            if candidate.is_file():
                return candidate.resolve()

    return None


def list_available_midori_exes(repo: Path) -> list[tuple[str, Path]]:
    results: list[tuple[str, Path]] = []

    # Check build/out/ first (CMake command-line builds)
    for label, subdir in [("build/out", "out"), ("build/Release", "Release"), ("build", "")]:
        if subdir:
            candidate = repo / "build" / subdir / "marmotc.exe"
        else:
            candidate = repo / "build" / "marmotc.exe"
        if candidate.is_file():
            results.append((label, candidate.resolve()))

    for preset_name in ["x64-release", "x64-development", "x64-debug"]:
        for candidate in get_preset_executable_candidates(repo, preset_name):
            if candidate.is_file():
                results.append((preset_name, candidate.resolve()))
                break

    return results


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
        raise RuntimeError("Scope 'machine' requires running the installer as Administrator.")


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


def read_windows_env(scope: str, name: str) -> tuple[str, int]:
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
            return "", winreg.REG_EXPAND_SZ

    if not isinstance(value, str):
        return "", value_type
    return value, value_type


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


def write_install_marker(target_layout: InstallLayout, scope: str) -> None:
    marker_path = target_layout.root / INSTALL_MARKER_FILENAME
    marker = {
        "schema_version": 1,
        "installed_by": "scripts/install.py",
        "scope": scope,
        "prelude_dir": str(target_layout.prelude_dir),
        "bin_dir": str(target_layout.bin_dir),
    }
    marker_path.write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")


def find_tool_exe(repo: Path) -> Optional[Path]:
    executable_name = "marmot.exe" if is_windows() else "marmot"
    for profile in ["release", "debug"]:
        candidate = repo / "tool" / "target" / profile / executable_name
        if candidate.is_file():
            return candidate.resolve()
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Install Marmot and configure MARMOT_PATH / PATH.")
    parser.add_argument("--scope", choices=["user", "machine"], default="user", help="Target environment scope (default: user).")
    parser.add_argument("--install-dir", default="", help="Installation directory (defaults to LocalAppData/ProgramFiles based on scope).")
    parser.add_argument("--preset", default="auto", help="CMake preset to locate marmotc.exe (default: auto).")
    parser.add_argument("--marmot-exe", default="", help="Explicit path to marmotc.exe (overrides --preset).")
    parser.add_argument("--copy-binaries", action="store_true", help="Install marmotc, marmotvm and the marmot tool, and add the bin dir to PATH.")
    args = parser.parse_args(argv)

    require_admin_for_machine(args.scope)

    repo = repo_root()
    prelude_src = repo / "MarmotPrelude"
    if not prelude_src.is_dir():
        raise RuntimeError(f"Prelude source directory not found: {prelude_src}")

    target_layout = layout(args.scope, args.install_dir if args.install_dir != "" else None)

    target_layout.root.mkdir(parents=True, exist_ok=True)
    copy_dir_contents(prelude_src, target_layout.prelude_dir)

    if args.copy_binaries:
        exe_path = resolve_midori_exe(repo, args.preset, args.marmot_exe if args.marmot_exe != "" else None)
        if exe_path is None or not exe_path.is_file():
            raise RuntimeError("marmotc.exe not found (build first, pass --marmot-exe, or use --preset).")

        if args.preset == "auto" and args.marmot_exe == "":
            available = list_available_midori_exes(repo)
            available_presets = list(dict.fromkeys(preset_name for preset_name, _ in available))
            if len(available_presets) > 1:
                print(
                    "Note: multiple builds found ("
                    + ", ".join(available_presets)
                    + "); auto selected the first match. Use --preset to force a specific build.",
                    file=sys.stderr,
                )

        print(f"Using marmotc.exe from: {exe_path}")

        target_layout.bin_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe_path, target_layout.bin_dir / "marmotc.exe")

        vm_path = exe_path.with_name("marmotvm.exe")
        if not vm_path.is_file():
            raise RuntimeError(f"marmotvm.exe not found beside {exe_path}; build the marmotvm target.")
        print(f"Using marmotvm.exe from: {vm_path}")
        shutil.copy2(vm_path, target_layout.bin_dir / "marmotvm.exe")

        tool_path = find_tool_exe(repo)
        if tool_path is None:
            print(
                "Note: the marmot project tool is not built, so only marmotc was installed. "
                "Build it with: cargo build --release --manifest-path tool/Cargo.toml",
                file=sys.stderr,
            )
        else:
            print(f"Using marmot from: {tool_path}")
            shutil.copy2(tool_path, target_layout.bin_dir / tool_path.name)

    write_install_marker(target_layout, args.scope)

    if is_windows():
        marmot_path_value, marmot_path_type = read_windows_env(args.scope, "MARMOT_PATH")
        updated_midori_path = append_unique_path(marmot_path_value, target_layout.prelude_dir)
        write_windows_env(args.scope, "MARMOT_PATH", updated_midori_path, marmot_path_type)
        os.environ["MARMOT_PATH"] = updated_midori_path

        if args.copy_binaries:
            path_value, path_type = read_windows_env(args.scope, "PATH")
            updated_path = append_unique_path(path_value, target_layout.bin_dir)
            write_windows_env(args.scope, "PATH", updated_path, path_type)
            os.environ["PATH"] = updated_path

        broadcast_environment_change_windows()
    else:
        print("Non-Windows install: copied files, but did not persist environment variables.", file=sys.stderr)
        print(f"Add this to your shell configuration:\n  export MARMOT_PATH=\"{target_layout.prelude_dir}\"", file=sys.stderr)
        if args.copy_binaries:
            print(f"  export PATH=\"$PATH:{target_layout.bin_dir}\"", file=sys.stderr)

    print(f"Installed MarmotPrelude to: {target_layout.prelude_dir}")
    if args.copy_binaries:
        print(f"Installed binaries to: {target_layout.bin_dir}")
        print("You may need to open a new terminal for PATH changes to take effect.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
