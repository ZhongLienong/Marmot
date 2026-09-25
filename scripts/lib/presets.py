"""Build trees: the CMake preset for a configuration, where it builds, and what it builds."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from lib import toolchain
from lib.host import REPO_ROOT, executable, preset_family

BUILD_TYPES = ["Debug", "Development", "Release", "Experimental"]


def _configure_presets() -> dict[str, dict[str, Any]]:
    contents = json.loads((REPO_ROOT / "CMakePresets.json").read_text(encoding="utf-8-sig"))
    return {str(preset["name"]): preset for preset in contents.get("configurePresets", [])}


def _resolved(name: str, presets: dict[str, dict[str, Any]]) -> dict[str, Any]:
    if name not in presets:
        raise SystemExit(f"No CMake preset '{name}'. Presets: {', '.join(sorted(presets))}")
    preset = presets[name]
    inherited = preset.get("inherits", [])
    resolved: dict[str, Any] = {}
    for parent in [inherited] if isinstance(inherited, str) else inherited:
        resolved.update(_resolved(parent, presets))
    resolved.update(preset)
    return resolved


@dataclass(frozen=True)
class BuildTree:
    preset: str
    build_type: str
    binary_dir: Path

    @staticmethod
    def select(build_type: str = "Development", preset: str | None = None) -> "BuildTree":
        presets = _configure_presets()
        name = preset if preset else preset_family() + build_type.lower()
        resolved = _resolved(name, presets)
        binary_dir = str(resolved["binaryDir"]).replace("${sourceDir}", str(REPO_ROOT)).replace("${presetName}", name)
        chosen_type = resolved.get("cacheVariables", {}).get("CMAKE_BUILD_TYPE", build_type)
        return BuildTree(name, chosen_type, Path(binary_dir).resolve())

    @staticmethod
    def from_args(args: argparse.Namespace) -> "BuildTree":
        return BuildTree.select(args.build, args.preset)

    @property
    def out_dir(self) -> Path:
        return self.binary_dir / "out"

    @property
    def compiler(self) -> Path:
        return self.out_dir / executable("marmotc")

    @property
    def vm(self) -> Path:
        return self.out_dir / executable("marmotvm")

    @property
    def unit_tests(self) -> Path:
        return self.out_dir / executable("MarmotUnitTests")

    @property
    def is_configured(self) -> bool:
        return (self.binary_dir / "CMakeCache.txt").is_file()

    def configure(self, *, with_unit_tests: bool = False, fresh: bool = False) -> int:
        command = ["cmake", "--preset", self.preset]
        if fresh:
            command.append("--fresh")
        # Release leaves the unit tests out unless asked for.
        if with_unit_tests and self.build_type == "Release":
            command.append("-DMIDORI_BUILD_TESTS=ON")
        return toolchain.run(command, environment=toolchain.build_environment(configuring=fresh or not self.is_configured))

    def build(self, targets: list[str]) -> int:
        if not self.is_configured:
            configured = self.configure(with_unit_tests="MarmotUnitTests" in targets)
            if configured != 0:
                return configured
        return toolchain.run(["cmake", "--build", "--preset", self.preset, "--target", *targets], environment=toolchain.build_environment())

    def require_compiler(self) -> Path:
        if not self.compiler.is_file():
            raise SystemExit(f"{self.compiler} is not built. Build it with: python scripts/dev.py build --build {self.build_type}")
        return self.compiler


def newest_built() -> BuildTree | None:
    """The host's build tree whose marmotc was built last: the one being worked on."""
    built = [tree for tree in map(BuildTree.select, BUILD_TYPES) if tree.compiler.is_file()]
    return max(built, key=lambda tree: tree.compiler.stat().st_mtime, default=None)


def add_build_arguments(parser: argparse.ArgumentParser, default: str | None = "Development") -> None:
    described = default if default else "the one built last"
    parser.add_argument("--build", choices=BUILD_TYPES, default=default, help=f"Build configuration (default: {described}).")
    parser.add_argument("--preset", default=None, help="A CMake preset by name, instead of the host's preset for --build.")
