#!/usr/bin/env python3
"""setup-ui's normal verifier: Python lint, C++ build, clang-tidy, CTest."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TIDY_SOURCES = (
    "src/session.cpp",
    "src/view.cpp",
    "src/rml_sdl_renderer.cpp",
    "tests/test_setup_ui.cpp",
)


def run(command: list[str]) -> None:
    print("verify:", " ".join(command))
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> int:
    run([sys.executable, "-m", "ruff", "format", "--check", "tools"])
    run([sys.executable, "-m", "ruff", "check", "tools"])

    build = ROOT / "build"
    if not (build / "CMakeCache.txt").is_file():
        run(
            [
                "cmake",
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                "-DCMAKE_CXX_COMPILER=clang++",
                "-DCMAKE_BUILD_TYPE=Debug",
            ]
        )
    run(["cmake", "--build", "build"])
    if not shutil.which("clang-tidy"):
        raise SystemExit("verify: clang-tidy is required")
    run(["clang-tidy", "-p", "build", *[str(ROOT / path) for path in TIDY_SOURCES]])
    run(["ctest", "--test-dir", "build", "--output-on-failure"])
    print("verify: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
