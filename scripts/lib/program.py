"""Building a Marmot program with marmotc and running it in marmotvm."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


def vm_beside(compiler: Path) -> Path:
    """marmotvm, built beside marmotc."""
    return compiler.with_name("marmotvm" + compiler.suffix)


def build_and_run(
    compiler: Path,
    source: str,
    *,
    cwd: Path,
    env: dict[str, str],
    timeout: float,
    program_args: list[str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Build `source` with marmotc and run it in marmotvm, as `marmot run` does.

    The result's output is the build's, then the run's; its exit code is the
    build's when the build failed. A timeout covers each step.
    """
    with tempfile.TemporaryDirectory(prefix="marmot-run-") as directory:
        program = Path(directory) / (Path(source).stem + ".mmc")
        options = dict(capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout, cwd=cwd, env=env, check=False)
        built = subprocess.run([str(compiler), "build", source, "-o", str(program), "--quiet"], **options)
        if built.returncode != 0:
            return built
        ran = subprocess.run([str(vm_beside(compiler)), str(program), *(program_args or [])], **options)
        return subprocess.CompletedProcess(ran.args, ran.returncode, built.stdout + ran.stdout, built.stderr + ran.stderr)
