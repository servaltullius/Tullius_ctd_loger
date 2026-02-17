import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    package_py = repo_root / "scripts" / "package.py"

    with tempfile.TemporaryDirectory(prefix="skydiag_pkg_test_") as td:
        td_path = Path(td)
        build_dir = td_path / "build"
        winui_dir = td_path / "winui"
        out_zip = td_path / "out.zip"

        # Minimal fake build artifacts expected by scripts/package.py
        _touch(build_dir / "bin" / "SkyrimDiag.dll")
        _touch(build_dir / "bin" / "SkyrimDiagHelper.exe")
        _touch(build_dir / "bin" / "SkyrimDiagDumpToolNative.dll")
        _touch(build_dir / "bin" / "SkyrimDiagDumpToolCli.exe")

        # Minimal fake WinUI publish folder.
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.exe")
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.pri")
        _touch(winui_dir / "App.xbf")
        _touch(winui_dir / "MainWindow.xbf")

        proc = subprocess.run(
            [
                sys.executable,
                str(package_py),
                "--build-dir",
                str(build_dir),
                "--winui-dir",
                str(winui_dir),
                "--out",
                str(out_zip),
                "--no-pdb",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            raise AssertionError(f"package.py failed ({proc.returncode})\\nstdout:\\n{proc.stdout}\\nstderr:\\n{proc.stderr}")

        assert out_zip.is_file(), "package.py did not produce zip"

        with zipfile.ZipFile(out_zip, "r") as zf:
            names = set(zf.namelist())

        assert (
            "SKSE/Plugins/SkyrimDiagDumpToolCli.exe" in names
        ), "Expected headless CLI exe to be packaged next to helper"
        assert (
            "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.pri" in names
        ), "Expected WinUI PRI asset to be packaged"
        assert (
            "SKSE/Plugins/SkyrimDiagWinUI/App.xbf" in names
        ), "Expected WinUI App.xbf asset to be packaged"
        assert (
            "SKSE/Plugins/SkyrimDiagWinUI/MainWindow.xbf" in names
        ), "Expected WinUI MainWindow.xbf asset to be packaged"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
