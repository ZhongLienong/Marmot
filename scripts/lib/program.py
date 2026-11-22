"""Building a Marmot program with marmotc and running it in marmotvm."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path

from lib.host import shared_library


def vm_beside(compiler: Path) -> Path:
    """The matching independent VM build, or an installed sibling executable."""
    if os.environ.get("MARMOTVM"):
        return Path(os.environ["MARMOTVM"])
    if compiler.parent.parent.parent.name == "marmotc":
        return compiler.parent.parent.parent.parent / "marmotvm" / compiler.parent.parent.name / "out" / ("marmotvm" + compiler.suffix)
    return compiler.with_name("marmotvm" + compiler.suffix)


def stdlib_beside(vm: Path) -> Path:
    if os.environ.get("MARMOTSTDLIB"):
        return Path(os.environ["MARMOTSTDLIB"])
    name = shared_library("marmotstdlib")
    project = vm.parent.parent.parent
    if project.name == "marmotvm":
        return project.parent / "marmotstdlib" / vm.parent.parent.name / "out" / name
    return vm.with_name(name)

def checkout_build_plan(path: Path, cwd: Path, env: dict[str, str]) -> list[str]:
    searches = [str((cwd / entry).resolve()) for entry in env.get("MARMOT_PATH", "").split(os.pathsep) if entry]
    plan = {"version": 1, "search_paths": searches,
            "native_libraries": [{"name": "marmotstdlib", "thread_safe": True}]}
    path.write_text(json.dumps(plan), encoding="utf-8")
    return ["--plan", str(path)]

def stdlib_override(env: dict[str, str]) -> list[str]:
    return ["--library", f"marmotstdlib={env['MARMOTSTDLIB']}"] if env.get("MARMOTSTDLIB") else []


def build_and_run(
    compiler: Path,
    source: str,
    *,
    cwd: Path,
    env: dict[str, str],
    timeout: float,
    program_args: list[str] | None = None,
    compiler_args: list[str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Build `source` with marmotc and run it in marmotvm, as `marmot run` does.

    The result's stdout is all the build printed, its diagnostics included,
    then the run's stdout; its stderr is the run's. Its exit code is the
    build's when the build failed. A timeout covers each step. `compiler_args`
    go to `marmotc build` after the source.
    """
    with tempfile.TemporaryDirectory(prefix="marmot-run-") as directory:
        program = Path(directory) / (Path(source).stem + ".mmc")
        options = dict(capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout, cwd=cwd, env=env, check=False)
        arguments = compiler_args or []
        if "--plan" not in arguments:
            arguments = [*arguments, *checkout_build_plan(Path(directory) / "plan.json", cwd, env)]
        built = subprocess.run([str(compiler), "build", source, "-o", str(program), "--quiet", *arguments], **options)
        if built.returncode != 0:
            return built
        ran = subprocess.run([str(vm_beside(compiler)), "run", str(program), *stdlib_override(env), *(program_args or [])], **options)
        return subprocess.CompletedProcess(ran.args, ran.returncode, built.stdout + built.stderr + ran.stdout, ran.stderr)
