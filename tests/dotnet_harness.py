#!/usr/bin/env python3
"""Runs a .NET test harness: built once per test process, then started from
its build output.

`dotnet run` built the project again on every call (about 2 s each), and every
MSBuild run left two empty folders and a `Microsoft.NET.Workload_*.log` in the
temp folder: about 80 folders and 40 logs per suite run, never removed. The one
build here uses a private temp folder that is removed with the build output
when the test process ends. Under WSL `dotnet` starts the Windows dotnet.exe,
which does not see a Linux TEMP, so that build still uses the Windows temp
folder (once per test process instead of once per case).
"""
from __future__ import annotations

import atexit
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _in_wsl() -> bool:
    return bool(os.environ.get("WSL_DISTRO_NAME"))


def dotnet_path(path: Path) -> str:
    # Under WSL `dotnet` may be the Windows host SDK, which needs Windows paths.
    if _in_wsl():
        return subprocess.run(
            ["wslpath", "-w", str(path)],
            cwd=str(REPO_ROOT),
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
    return str(path)


class Harness:
    def __init__(self, project: Path, framework: str = "net8.0") -> None:
        self._project = project
        self._framework = framework
        self._program: Path | None = None
        self._env: dict[str, str] = dict(os.environ)

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        program = self._build()
        return subprocess.run(
            ["dotnet", dotnet_path(program), *args],
            cwd=str(REPO_ROOT),
            env=self._env,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    def _build(self) -> Path:
        if self._program is not None:
            return self._program
        root = Path(tempfile.mkdtemp(prefix="skydiag_dotnet_harness_"))
        atexit.register(shutil.rmtree, root, ignore_errors=True)
        out = root / "out"
        if not _in_wsl():
            temp = root / "temp"
            temp.mkdir()
            self._env.update(TEMP=str(temp), TMP=str(temp), TMPDIR=str(temp))
        completed = subprocess.run(
            [
                "dotnet",
                "build",
                dotnet_path(self._project),
                "--framework",
                self._framework,
                # A build server started now would keep the private temp folder
                # as its TEMP after this process removed it.
                "--disable-build-servers",
                "--output",
                dotnet_path(out),
                "-nologo",
                "-v",
                "q",
            ],
            cwd=str(REPO_ROOT),
            env=self._env,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"dotnet build failed for {self._project.name}\n"
                f"stdout:\n{completed.stdout}\n"
                f"stderr:\n{completed.stderr}"
            )
        self._program = out / f"{self._project.stem}.dll"
        return self._program
