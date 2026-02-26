import importlib.util
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"


def _load_release_contract():
    contract_path = SCRIPTS_DIR / "release_contract.py"
    spec = importlib.util.spec_from_file_location("release_contract", contract_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load release contract module: {contract_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_RELEASE_CONTRACT = _load_release_contract()
REQUIRED_ZIP_ENTRIES = tuple(_RELEASE_CONTRACT.REQUIRED_ZIP_ENTRIES)
EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset(
    _RELEASE_CONTRACT.EXCLUDED_WINUI_TOP_LEVEL_DIRS
)


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x")


def main() -> int:
    package_py = REPO_ROOT / "scripts" / "package.py"

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
        # Nested build/publish output should be ignored by the packager.
        _touch(winui_dir / "publish" / "SkyrimDiagDumpToolWinUI.exe")
        _touch(winui_dir / "win-x64" / "SkyrimDiagDumpToolWinUI.exe")

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
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            raise AssertionError(
                f"package.py failed ({proc.returncode})\\nstdout:\\n{proc.stdout}\\nstderr:\\n{proc.stderr}"
            )

        assert out_zip.is_file(), "package.py did not produce zip"

        with zipfile.ZipFile(out_zip, "r") as zf:
            names = set(zf.namelist())

        for entry in REQUIRED_ZIP_ENTRIES:
            assert entry in names, f"Missing required zip entry in zip: {entry}"

        assert "SKSE/Plugins/SkyrimDiag.ini" in names, (
            "Expected SkyrimDiag.ini to be packaged"
        )
        assert "SKSE/Plugins/SkyrimDiagHelper.ini" in names, (
            "Expected SkyrimDiagHelper.ini to be packaged"
        )

        for nested_dir in sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS):
            forbidden_prefix = f"SKSE/Plugins/SkyrimDiagWinUI/{nested_dir}/"
            assert not any(name.startswith(forbidden_prefix) for name in names), (
                f"Nested WinUI output must not be packaged: {forbidden_prefix}"
            )

        data_root = REPO_ROOT / "dump_tool" / "data"
        expected_data = [
            p.relative_to(data_root).as_posix()
            for p in data_root.rglob("*")
            if p.is_file()
        ]
        assert expected_data, "Expected at least one dump_tool/data file in repository"
        for rel in expected_data:
            plugin_data_path = f"SKSE/Plugins/data/{rel}"
            winui_data_path = f"SKSE/Plugins/SkyrimDiagWinUI/data/{rel}"
            assert plugin_data_path in names, (
                f"Missing plugin data file in zip: {plugin_data_path}"
            )
            assert winui_data_path in names, (
                f"Missing WinUI data file in zip: {winui_data_path}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
