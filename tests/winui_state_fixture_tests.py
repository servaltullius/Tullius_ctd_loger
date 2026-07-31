#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PROJECT = REPO_ROOT / "tests" / "winui_state_fixture_harness" / "WinUIStateFixtureHarness.csproj"


def _dotnet_path(path: Path) -> str:
    if os.environ.get("WSL_DISTRO_NAME"):
        return subprocess.run(
            ["wslpath", "-w", str(path)],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
    return str(path)


def main() -> None:
    completed = subprocess.run(
        [
            "dotnet",
            "run",
            "--project",
            _dotnet_path(PROJECT),
            "--framework",
            "net8.0",
        ],
        cwd=str(REPO_ROOT),
        text=True,
        encoding="utf-8",
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "WinUI state harness failed\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    assert "winui_state_fixture_harness: OK" in completed.stdout
    print("winui_state_fixture_tests: OK")


if __name__ == "__main__":
    main()
