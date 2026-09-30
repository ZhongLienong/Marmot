"""Independent CMake build trees orchestrated with one profile selection."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from lib import toolchain
from lib.host import REPO_ROOT, executable, preset_family

BUILD_TYPES = ["Debug", "Dev", "Release"]
PROJECTS = ("marmotc", "marmotvm")
UNIT_TARGETS = ("MarmotcUnitTests", "MarmotvmUnitTests", "MarmotIntegrationTests")


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
class ProjectBuild:
    project: str
    preset: str
    build_type: str
    binary_dir: Path

    @property
    def source_dir(self) -> Path:
        return REPO_ROOT / "projects" / self.project

    @property
    def is_configured(self) -> bool:
        return (self.binary_dir / "CMakeCache.txt").is_file()

    def configure(self, *, with_unit_tests: bool = False, fresh: bool = False) -> int:
        command = ["cmake", "--preset", self.preset]
        if fresh:
            command.append("--fresh")
        if with_unit_tests:
            command.append("-DMIDORI_BUILD_TESTS=ON")
            if self.project == "marmotc":
                command.append("-DMIDORI_BUILD_INTEGRATION_TESTS=ON")
        return toolchain.run(command, cwd=self.source_dir,
                             environment=toolchain.build_environment(configuring=fresh or not self.is_configured))


@dataclass(frozen=True)
class BuildTree:
    preset: str
    build_type: str
    projects: tuple[ProjectBuild, ...]

    @staticmethod
    def select(build_type: str = "Dev", preset: str | None = None) -> "BuildTree":
        name = preset or preset_family() + build_type.lower()
        builds = []
        for project in PROJECTS:
            source = REPO_ROOT / "projects" / project
            contents = json.loads((source / "CMakePresets.json").read_text(encoding="utf-8-sig"))
            resolved = _resolved(name, {entry["name"]: entry for entry in contents["configurePresets"]})
            directory = str(resolved["binaryDir"]).replace("${sourceDir}", str(source)).replace("${presetName}", name)
            configuration = resolved.get("cacheVariables", {}).get("CMAKE_BUILD_TYPE", build_type)
            chosen_type = "Dev" if configuration == "RelWithDebInfo" else configuration
            builds.append(ProjectBuild(project, name, chosen_type, Path(directory).resolve()))
        return BuildTree(name, builds[0].build_type, tuple(builds))

    @staticmethod
    def from_args(args: argparse.Namespace) -> "BuildTree":
        return BuildTree.select(args.build, args.preset)

    @property
    def binary_dir(self) -> Path:
        return self.projects[0].binary_dir

    @property
    def out_dir(self) -> Path:
        return self.binary_dir / "out"

    @property
    def compiler(self) -> Path:
        return self.out_dir / executable("marmotc")

    @property
    def vm(self) -> Path:
        return self.projects[1].binary_dir / "out" / executable("marmotvm")

    @property
    def unit_tests(self) -> tuple[Path, ...]:
        return (self.out_dir / executable(UNIT_TARGETS[0]),
                self.projects[1].binary_dir / "out" / executable(UNIT_TARGETS[1]))

    @property
    def integration_tests(self) -> Path:
        return self.out_dir / executable(UNIT_TARGETS[2])

    @property
    def is_configured(self) -> bool:
        return all(project.is_configured for project in self.projects)

    def configure(self, *, with_unit_tests: bool = False, fresh: bool = False) -> int:
        for project in self.projects:
            status = project.configure(with_unit_tests=with_unit_tests, fresh=fresh)
            if status != 0:
                return status
        return 0

    def build(self, targets: list[str]) -> int:
        for project in self.projects:
            chosen = list(dict.fromkeys(target for target in targets
                          if (target in {"marmotvm", "MarmotvmUnitTests"}) == (project.project == "marmotvm")))
            if not chosen:
                continue
            tests = any(target in UNIT_TARGETS for target in chosen)
            if tests or not project.is_configured:
                status = project.configure(with_unit_tests=tests)
                if status != 0:
                    return status
            status = toolchain.run(["cmake", "--build", "--preset", self.preset, "--target", *chosen],
                                   cwd=project.source_dir, environment=toolchain.build_environment())
            if status != 0:
                return status
        return 0

    def require_compiler(self) -> Path:
        if not self.compiler.is_file():
            raise SystemExit(f"{self.compiler} is not built. Build it with: python scripts/dev.py build --build {self.build_type}")
        return self.compiler


def newest_built() -> BuildTree | None:
    built = [tree for tree in map(BuildTree.select, BUILD_TYPES) if tree.compiler.is_file()]
    return max(built, key=lambda tree: max(path.stat().st_mtime for path in (tree.compiler, tree.vm) if path.is_file()), default=None)


def add_build_arguments(parser: argparse.ArgumentParser, default: str | None = "Dev") -> None:
    described = default if default else "the one built last"
    parser.add_argument("--build", choices=BUILD_TYPES, default=default, help=f"Build configuration (default: {described}).")
    parser.add_argument("--preset", default=None, help="The matching preset in each C++ project.")
