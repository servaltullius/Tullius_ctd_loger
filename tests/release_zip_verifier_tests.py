#!/usr/bin/env python3
"""Behavioral tests for scripts/verify_release_zip.py."""

from __future__ import annotations

import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"
VERIFIER = SCRIPTS_DIR / "verify_release_zip.py"


def _load_release_contract():
    path = SCRIPTS_DIR / "release_contract.py"
    spec = importlib.util.spec_from_file_location("release_contract_for_zip_tests", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load release contract: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CONTRACT = _load_release_contract()


def _minimal_pe(machine: int = 0x8664, marker: bytes = b"") -> bytes:
    blob = bytearray(512)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3C, 0x80)
    blob[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", blob, 0x84, machine)
    blob[0x100 : 0x100 + len(marker)] = marker
    return bytes(blob)


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _make_fixture(root: Path, *, zip_name: str = "Tullius_ctd_loger_v1.2.3.zip") -> tuple[Path, Path, Path]:
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24.0)\n"
        "project(SkyrimDiag VERSION 1.2.3 LANGUAGES CXX)\n",
        encoding="utf-8",
    )
    build_dir = root / "build-win"
    winui_dir = root / "build-winui"

    entries: dict[str, bytes] = {
        name: b"required-asset" for name in CONTRACT.REQUIRED_ZIP_ENTRIES
    }
    for index, (filename, entry) in enumerate(CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS):
        data = _minimal_pe(marker=f"native-{index}".encode("ascii"))
        _write(build_dir / "bin" / filename, data)
        entries[entry] = data

    for index, (filename, entry) in enumerate(CONTRACT.WINUI_BUILD_ZIP_MAPPINGS):
        data = _minimal_pe(marker=f"winui-{index}".encode("ascii"))
        _write(winui_dir / filename, data)
        entries[entry] = data

    for asset in CONTRACT.REQUIRED_WINUI_BUILD_OUTPUTS:
        path = winui_dir / asset
        if not path.exists():
            _write(path, b"winui-asset")

    zip_path = root / zip_name
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return zip_path, build_dir, winui_dir


def _run(root: Path, zip_path: Path, build_dir: Path, winui_dir: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(VERIFIER),
            "--repo-root",
            str(root),
            "--zip",
            str(zip_path),
            "--build-dir",
            str(build_dir),
            "--winui-dir",
            str(winui_dir),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )


def test_valid_release_zip_passes() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_ok_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr
        assert "archive matches current native and WinUI builds" in result.stdout


def test_pdb_entry_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_pdb_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "a") as archive:
            archive.writestr("SKSE/Plugins/SkyrimDiag.pdb", b"symbols")
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "PDB must not be shipped" in result.stderr


def test_matching_prerelease_suffix_passes() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_rc_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(
            root, zip_name="Tullius_ctd_loger_v1.2.3-rc15.zip"
        )
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr


def test_non_x64_entry_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_x86_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        entries[CONTRACT.REQUIRED_X64_PE_ZIP_ENTRIES[0]] = _minimal_pe(0x014C)
        with zipfile.ZipFile(zip_path, "w") as archive:
            for name, data in entries.items():
                archive.writestr(name, data)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "non-x64 PE entry" in result.stderr


def test_versioned_filename_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_version_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(
            root, zip_name="Tullius_ctd_loger_v9.9.9.zip"
        )
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "release zip filename mismatch" in result.stderr


def test_zip_must_match_current_build() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_stale_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        filename, _ = CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS[0]
        _write(build_dir / "bin" / filename, _minimal_pe(marker=b"rebuilt"))
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "packaged file differs from current build" in result.stderr


def _write_vcpkg_manifest(root: Path, version: str | None) -> None:
    manifest: dict[str, object] = {"name": "skyrimdiag", "dependencies": []}
    if version is not None:
        manifest["version-string"] = version
    (root / "vcpkg.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def test_matching_vcpkg_version_passes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        _write_vcpkg_manifest(root, "1.2.3")
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr


def test_mismatched_vcpkg_version_fails() -> None:
    # vcpkg.json sat 14 releases behind CMakeLists.txt because nothing compared
    # them; the release path only ever read the CMake version.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        _write_vcpkg_manifest(root, "0.9.9")
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "version mismatch" in result.stderr


def test_absent_vcpkg_version_is_allowed() -> None:
    # Omitting the field is a valid way to keep CMakeLists.txt authoritative,
    # so it must not be treated as a mismatch.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        _write_vcpkg_manifest(root, None)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr


def main() -> int:
    test_valid_release_zip_passes()
    test_pdb_entry_fails()
    test_matching_prerelease_suffix_passes()
    test_non_x64_entry_fails()
    test_versioned_filename_mismatch_fails()
    test_zip_must_match_current_build()
    test_matching_vcpkg_version_passes()
    test_mismatched_vcpkg_version_fails()
    test_absent_vcpkg_version_is_allowed()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
