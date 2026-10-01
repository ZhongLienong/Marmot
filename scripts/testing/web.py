#!/usr/bin/env python3
"""Serve the actual browser modules and compatibility fixtures for the smoke test."""

from __future__ import annotations

import argparse
import functools
import http.server
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT
from make import wasm


def prepare() -> Path:
    directory = REPO_ROOT / "out" / "web-smoke"
    directory.mkdir(parents=True, exist_ok=True)
    wasm.deploy(directory)
    shutil.copy2(REPO_ROOT / "projects" / "web" / "tests" / "smoke.html", directory / "index.html")
    shutil.copytree(REPO_ROOT / "format" / "mmc" / "fixtures", directory / "fixtures", dirs_exist_ok=True)
    return directory


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args(argv)
    directory = prepare()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(directory))
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Browser smoke test: http://127.0.0.1:{args.port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
