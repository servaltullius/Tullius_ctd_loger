#!/usr/bin/env python3
"""Runs a .NET test harness: built once per test process, then started from
its build output.

`dotnet run` built the project again on every call (about 2 s each), and every
MSBuild run left two empty folders and a `Microsoft.NET.Workload_*.log` in the
temp folder: about 80 folders and 40 logs per suite run, never removed. Here
the project is built once into its own bin folder, as `dotnet run` did, with a
private temp folder under its obj folder that is removed right after the build.
Under WSL `dotnet` starts the Windows dotnet.exe, so that folder is handed over
as a Windows path through WSLENV. Running the built program leaves nothing.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CONFIGURATION = "Debug"


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


def _build_env(temp: Path) -> dict[str, str]:
    env = dict(os.environ)
    temp_arg = dotnet_path(temp)
    env.update(TEMP=temp_arg, TMP=temp_arg, TMPDIR=temp_arg)
    if _in_wsl():
        # The values are Windows paths already; /w passes them to Windows
        # programs only, unchanged.
        shared = [entry for entry in env.get("WSLENV", "").split(":") if entry]
        env["WSLENV"] = ":".join(shared + ["TEMP/w", "TMP/w"])
    return env


class Harness:
    def __init__(self, project: Path, framework: str = "net8.0") -> None:
        self._project = project
        self._framework = framework
        self._program: Path | None = None
        self._failure: RuntimeError | None = None

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        program = self._build()
        return subprocess.run(
            ["dotnet", dotnet_path(program), *args],
            cwd=str(REPO_ROOT),
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    def _build(self) -> Path:
        if self._program is not None:
            return self._program
        # A failed build fails every case the same way; report it, not rebuild.
        if self._failure is not None:
            raise self._failure
        obj = self._project.parent / "obj"
        obj.mkdir(parents=True, exist_ok=True)
        temp = Path(tempfile.mkdtemp(prefix="harness_temp_", dir=obj))
        try:
            completed = subprocess.run(
                [
                    "dotnet",
                    "build",
                    dotnet_path(self._project),
                    "--configuration",
                    CONFIGURATION,
                    "--framework",
                    self._framework,
                    # A build server started now would keep the private temp
                    # folder as its TEMP after it is removed.
                    "--disable-build-servers",
                    "-nologo",
                    "-v",
                    "q",
                ],
                cwd=str(REPO_ROOT),
                env=_build_env(temp),
                text=True,
                encoding="utf-8",
                capture_output=True,
                check=False,
            )
        finally:
            shutil.rmtree(temp, ignore_errors=True)
        if completed.returncode != 0:
            self._failure = RuntimeError(
                f"dotnet build failed for {self._project.name}\n"
                f"stdout:\n{completed.stdout}\n"
                f"stderr:\n{completed.stderr}"
            )
            raise self._failure
        self._program = (
            self._project.parent / "bin" / CONFIGURATION / self._framework / f"{self._project.stem}.dll"
        )
        return self._program
