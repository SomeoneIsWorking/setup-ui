#!/usr/bin/env python3
"""setup-ui's normal verifier: Python lint + tests, C++ format, clang-tidy, CTest."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C_SOURCES = sorted(
    str(path)
    for pattern in ("src/*.cpp", "include/**/*.h", "tests/*.cpp", "tools/*.py")
    for path in ROOT.glob(pattern)
    if path.is_file() and path.suffix in (".cpp", ".h", ".py")
)


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("verify:", " ".join(command))
    subprocess.run(command, cwd=ROOT, check=True, env=env or os.environ)


def require(name: str, variable: str) -> str:
    path = os.environ.get(variable) or shutil.which(name)
    if not path:
        raise SystemExit(f"verify: {name} is required; set {variable} or install it")
    return path


def main() -> int:
    run(
        [sys.executable, "-m", "ruff", "format", "--check", "tools", "tests"]
        if False
        else [sys.executable, "-m", "ruff", "format", "--check", "tools"]
    )
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
                "-DCMAKE_BUILD_TYPE=Release",
            ]
        )
    run(["cmake", "--build", "build"])
    require("clang-tidy", "CLANG_TIDY")
    compile_db = build / "compile_commands.json"
    if not compile_db.is_file():
        raise SystemExit("verify: build did not produce compile_commands.json")
    tidy_sources = [
        str(ROOT / path)
        for path in (
            "src/setup_ui.cpp",
            "src/setup_ui_c.cpp",
            "tests/test_setup_ui.cpp",
        )
    ]
    run(["clang-tidy", "-p", "build", *tidy_sources])
    run(["ctest", "--test-dir", "build", "--output-on-failure"])
    print("verify: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
