#!/usr/bin/env python3
"""Behavioral tests for scripts/verify_release_zip.py."""

from __future__ import annotations

import importlib.util
import hashlib
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


def _packed_version(version: tuple[int, int, int, int]) -> int:
    return (
        (version[0] & 0xFF) << 24
        | (version[1] & 0xFF) << 16
        | (version[2] & 0xFFF) << 4
        | (version[3] & 0xF)
    )


def _minimal_pe(
    machine: int = 0x8664,
    marker: bytes = b"",
    version: tuple[int, int, int, int] = (1, 2, 3, 0),
    *,
    arm64x_hybrid: bool = False,
    managed_flags: int | None = None,
    skse_plugin: bool = False,
    skse_version: tuple[int, int, int, int] | None = None,
) -> bytes:
    blob = bytearray(0x1000)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3C, 0x80)
    blob[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", blob, 0x84, machine)
    struct.pack_into("<H", blob, 0x86, 1)  # NumberOfSections
    is_pe32 = machine == 0x014C and managed_flags is not None
    optional_size = 0xE0 if is_pe32 else 0xF0
    directory_count_relative = 92 if is_pe32 else 108
    data_directory_relative = 96 if is_pe32 else 112
    struct.pack_into("<H", blob, 0x94, optional_size)
    struct.pack_into("<H", blob, 0x98, 0x10B if is_pe32 else 0x20B)
    image_base = 0x180000000
    if is_pe32:
        struct.pack_into("<I", blob, 0x98 + 28, 0x00400000)
    else:
        struct.pack_into("<Q", blob, 0x98 + 24, image_base)
    if arm64x_hybrid:
        struct.pack_into(
            "<I", blob, 0x98 + directory_count_relative, 11
        )  # NumberOfRvaAndSizes
        struct.pack_into(
            "<II",
            blob,
            0x98 + data_directory_relative + 10 * 8,
            0x1600,
            0xD0,
        )
        struct.pack_into("<I", blob, 0x800, 0xD0)
        struct.pack_into("<Q", blob, 0x800 + 200, image_base + 0x1700)
        struct.pack_into("<I", blob, 0x900, 2)
    if managed_flags is not None:
        struct.pack_into(
            "<I", blob, 0x98 + directory_count_relative, 16
        )  # NumberOfRvaAndSizes
        struct.pack_into(
            "<II",
            blob,
            0x98 + data_directory_relative + 14 * 8,
            0x1500,
            0x48,
        )
        struct.pack_into("<IHHII", blob, 0x700, 0x48, 2, 5, 0, 0)
        struct.pack_into("<I", blob, 0x710, managed_flags)

    section_offset = 0x98 + optional_size
    blob[section_offset : section_offset + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", blob, section_offset + 8, 0x800, 0x1000, 0x800, 0x200)

    if skse_plugin:
        export_rva = 0x1100
        struct.pack_into("<II", blob, 0x98 + 112, export_rva, 0x180)
        export_offset = 0x300
        struct.pack_into("<I", blob, export_offset + 16, 1)  # Base
        struct.pack_into("<I", blob, export_offset + 20, 1)  # NumberOfFunctions
        struct.pack_into("<I", blob, export_offset + 24, 1)  # NumberOfNames
        struct.pack_into("<I", blob, export_offset + 28, 0x1140)
        struct.pack_into("<I", blob, export_offset + 32, 0x1150)
        struct.pack_into("<I", blob, export_offset + 36, 0x1160)
        struct.pack_into("<I", blob, 0x340, 0x1200)
        struct.pack_into("<I", blob, 0x350, 0x1170)
        struct.pack_into("<H", blob, 0x360, 0)
        blob[0x370 : 0x370 + len(b"SKSEPlugin_Version\0")] = b"SKSEPlugin_Version\0"
        struct.pack_into(
            "<II",
            blob,
            0x400,
            1,
            _packed_version(skse_version if skse_version is not None else version),
        )

    file_ms = (version[0] << 16) | version[1]
    file_ls = (version[2] << 16) | version[3]
    struct.pack_into(
        "<IIIIII",
        blob,
        0x500,
        0xFEEF04BD,
        0x00010000,
        file_ms,
        file_ls,
        file_ms,
        file_ls,
    )
    blob[0x600 : 0x600 + len(marker)] = marker
    return bytes(blob)


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _git(root: Path, *args: str) -> None:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    assert result.returncode == 0, result.stderr


def _write_vcpkg_manifest(root: Path, version: str | None) -> None:
    manifest: dict[str, object] = {"name": "skyrimdiag", "dependencies": []}
    if version is not None:
        manifest["version-string"] = version
    (root / "vcpkg.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def _write_build_provenance(
    root: Path, build_dir: Path, winui_dir: Path
) -> None:
    writer = SCRIPTS_DIR / "write_build_provenance.py"
    native_args = [
        sys.executable,
        str(writer),
        "--repo-root",
        str(root),
        "--output",
        str(build_dir / "bin" / CONTRACT.BUILD_PROVENANCE_FILENAME),
        "--kind",
        "native",
        "--configuration",
        "RelWithDebInfo",
    ]
    for filename, _ in CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS:
        native_args.extend(
            ["--artifact", f"{filename}={build_dir / 'bin' / filename}"]
        )
    native = subprocess.run(native_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert native.returncode == 0, native.stderr.decode("utf-8", errors="replace")

    winui = subprocess.run(
        [
            sys.executable,
            str(writer),
            "--repo-root",
            str(root),
            "--output",
            str(winui_dir / CONTRACT.BUILD_PROVENANCE_FILENAME),
            "--kind",
            "winui",
            "--configuration",
            "Release",
            "--artifact-root",
            str(winui_dir),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert winui.returncode == 0, winui.stderr.decode("utf-8", errors="replace")


def _refresh_package_provenance(
    root: Path,
    build_dir: Path,
    winui_dir: Path,
    entries: dict[str, bytes],
) -> None:
    state = CONTRACT.source_state(root)
    package_provenance = {
        "schema": CONTRACT.PACKAGE_PROVENANCE_SCHEMA,
        "version": "1.2.3",
        **state,
        "native_configuration": "RelWithDebInfo",
        "winui_configuration": "Release",
        "native_build_provenance_sha256": CONTRACT.sha256_path(
            build_dir / "bin" / CONTRACT.BUILD_PROVENANCE_FILENAME
        ),
        "winui_build_provenance_sha256": CONTRACT.sha256_path(
            winui_dir / CONTRACT.BUILD_PROVENANCE_FILENAME
        ),
        "artifacts": {
            name: hashlib.sha256(data).hexdigest()
            for name, data in sorted(entries.items())
            if name != CONTRACT.PACKAGE_PROVENANCE_ENTRY
        },
    }
    entries[CONTRACT.PACKAGE_PROVENANCE_ENTRY] = (
        json.dumps(package_provenance, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def _write_zip(zip_path: Path, entries: dict[str, bytes]) -> None:
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def _make_fixture(
    root: Path,
    *,
    zip_name: str = "Tullius_ctd_loger_v1.2.3.zip",
    dirty: bool = False,
) -> tuple[Path, Path, Path]:
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24.0)\n"
        "project(SkyrimDiag VERSION 1.2.3 LANGUAGES CXX)\n",
        encoding="utf-8",
    )
    _write_vcpkg_manifest(root, "1.2.3")
    (root / ".gitignore").write_text(
        "build-win/\nbuild-winui/\n*.zip\n", encoding="utf-8"
    )
    _write(root / "dist" / "SkyrimDiag.ini", b"[SkyrimDiag]\n")
    _write(root / "dist" / "SkyrimDiagHelper.ini", b"[Helper]\n")
    _write(root / "dump_tool" / "data" / "fixture.json", b'{"fixture":true}\n')
    _git(root, "init")
    _git(root, "config", "user.email", "tests@example.invalid")
    _git(root, "config", "user.name", "SkyrimDiag Tests")
    _git(root, "add", ".")
    _git(root, "commit", "-m", "fixture")
    if dirty:
        with (root / "CMakeLists.txt").open("a", encoding="utf-8") as stream:
            stream.write("# intentionally dirty release fixture\n")

    build_dir = root / "build-win"
    winui_dir = root / "build-winui"
    (build_dir / "CMakeCache.txt").parent.mkdir(parents=True, exist_ok=True)
    (build_dir / "CMakeCache.txt").write_text(
        "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n", encoding="utf-8"
    )

    entries: dict[str, bytes] = {
        name: b"required-asset" for name in CONTRACT.REQUIRED_ZIP_ENTRIES
        if name != CONTRACT.PACKAGE_PROVENANCE_ENTRY
    }
    for index, (filename, entry) in enumerate(CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS):
        data = _minimal_pe(
            marker=f"native-{index}".encode("ascii"),
            skse_plugin=filename == "SkyrimDiag.dll",
        )
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
        entries[
            f"SKSE/Plugins/SkyrimDiagWinUI/app/{asset}"
        ] = path.read_bytes()

    extra_winui_pes = {
        "UnmappedNative.dll": _minimal_pe(marker=b"unmapped-native"),
        "AnyCpuManaged.dll": _minimal_pe(
            machine=0x014C,
            marker=b"anycpu-managed",
            managed_flags=0x00000001,
        ),
        "Arm64XHybrid.dll": _minimal_pe(
            machine=0xAA64,
            marker=b"arm64x-hybrid",
            arm64x_hybrid=True,
        ),
    }
    for filename, payload in extra_winui_pes.items():
        _write(winui_dir / filename, payload)
        entries[f"SKSE/Plugins/SkyrimDiagWinUI/app/{filename}"] = payload

    entries["SKSE/Plugins/SkyrimDiag.ini"] = (
        root / "dist" / "SkyrimDiag.ini"
    ).read_bytes()
    entries["SKSE/Plugins/SkyrimDiagHelper.ini"] = (
        root / "dist" / "SkyrimDiagHelper.ini"
    ).read_bytes()
    data = (root / "dump_tool" / "data" / "fixture.json").read_bytes()
    entries["SKSE/Plugins/data/fixture.json"] = data
    entries["SKSE/Plugins/SkyrimDiagWinUI/app/data/fixture.json"] = data

    _write_build_provenance(root, build_dir, winui_dir)

    _refresh_package_provenance(root, build_dir, winui_dir, entries)

    zip_path = root / zip_name
    _write_zip(zip_path, entries)
    return zip_path, build_dir, winui_dir


def _run(
    root: Path,
    zip_path: Path,
    build_dir: Path,
    winui_dir: Path,
    *,
    require_clean: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = [
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
    ]
    if require_clean:
        command.append("--require-clean")
    return subprocess.run(
        command,
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
        assert "archive matches exact native/WinUI builds" in result.stdout


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
        entry = CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS[0][1]
        entries[entry] = _minimal_pe(0x014C)
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "non-x64 PE entry" in result.stderr


def test_all_pe_entries_include_unmapped_native_files() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_all_pe_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr
        assert (
            "x64 native PE: "
            "SKSE/Plugins/SkyrimDiagWinUI/app/UnmappedNative.dll"
        ) in result.stdout
        assert (
            "architecture-neutral managed PE: "
            "SKSE/Plugins/SkyrimDiagWinUI/app/AnyCpuManaged.dll"
        ) in result.stdout
        assert (
            "x64-compatible Arm64X PE: "
            "SKSE/Plugins/SkyrimDiagWinUI/app/Arm64XHybrid.dll"
        ) in result.stdout


def test_unmapped_native_x86_pe_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_unmapped_x86_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        filename = "UnmappedNative.dll"
        entry = f"SKSE/Plugins/SkyrimDiagWinUI/app/{filename}"
        payload = _minimal_pe(machine=0x014C, marker=b"tampered-x86")
        _write(winui_dir / filename, payload)
        entries[entry] = payload
        _write_build_provenance(root, build_dir, winui_dir)
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert f"non-x64 PE entry: {entry}" in result.stderr


def test_32bit_required_managed_pe_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_managed_x86_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        filename = "AnyCpuManaged.dll"
        entry = f"SKSE/Plugins/SkyrimDiagWinUI/app/{filename}"
        payload = _minimal_pe(
            machine=0x014C,
            marker=b"managed-32bit-required",
            managed_flags=0x00000003,
        )
        _write(winui_dir / filename, payload)
        entries[entry] = payload
        _write_build_provenance(root, build_dir, winui_dir)
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert f"non-x64 PE entry: {entry}" in result.stderr
        assert "CLR flags=0x00000003" in result.stderr


def test_plain_arm64_pe_fails_without_arm64x_metadata() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_plain_arm64_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        filename = "Arm64XHybrid.dll"
        entry = f"SKSE/Plugins/SkyrimDiagWinUI/app/{filename}"
        payload = _minimal_pe(
            machine=0xAA64,
            marker=b"plain-arm64-tamper",
            arm64x_hybrid=False,
        )
        _write(winui_dir / filename, payload)
        entries[entry] = payload
        _write_build_provenance(root, build_dir, winui_dir)
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert f"non-x64 PE entry: {entry}" in result.stderr
        assert "machine=0xaa64" in result.stderr


def test_versioned_filename_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_version_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(
            root, zip_name="Tullius_ctd_loger_v9.9.9.zip"
        )
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "release zip filename mismatch" in result.stderr


def test_binary_version_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_pe_version_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        entry = CONTRACT.PROJECT_VERSIONED_PE_ZIP_ENTRIES[0]
        entries[entry] = _minimal_pe(
            version=(9, 9, 9, 0),
            skse_plugin=entry == "SKSE/Plugins/SkyrimDiag.dll",
        )
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "binary version mismatch" in result.stderr, result.stderr


def test_skse_metadata_version_mismatch_fails() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_skse_version_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        plugin_entry = "SKSE/Plugins/SkyrimDiag.dll"
        entries[plugin_entry] = _minimal_pe(
            skse_plugin=True,
            skse_version=(1, 2, 2, 0),
        )
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "SKSE plugin metadata version mismatch" in result.stderr


def test_package_must_bind_exact_build_manifests() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_manifest_hash_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        manifest = json.loads(
            entries[CONTRACT.PACKAGE_PROVENANCE_ENTRY].decode("utf-8")
        )
        manifest["native_build_provenance_sha256"] = "0" * 64
        entries[CONTRACT.PACKAGE_PROVENANCE_ENTRY] = (
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "package provenance build manifest mismatch" in result.stderr


def test_all_winui_build_files_are_bound_to_zip() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_xbf_bind_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        entry = "SKSE/Plugins/SkyrimDiagWinUI/app/App.xbf"
        assert entry not in {
            mapped for _, mapped in CONTRACT.WINUI_BUILD_ZIP_MAPPINGS
        }
        entries[entry] = b"self-recorded but not the built XBF"
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "WinUI build-to-package hash mismatch" in result.stderr


def test_build_provenance_requires_exact_artifact_coverage() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_build_coverage_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        manifest_path = winui_dir / CONTRACT.BUILD_PROVENANCE_FILENAME
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["artifacts"]["stale-output.dll"] = "0" * 64
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "build provenance coverage mismatch" in result.stderr


def test_ini_bytes_are_bound_to_source() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_ini_bind_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        entries["SKSE/Plugins/SkyrimDiag.ini"] = b"[tampered]\n"
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "packaged file differs from current build" in result.stderr


def test_static_data_bytes_are_bound_to_source() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_data_bind_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        entries["SKSE/Plugins/data/fixture.json"] = b'{"fixture":false}\n'
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "packaged file differs from current build" in result.stderr


def test_package_winui_configuration_is_bound() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_winui_config_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        manifest = json.loads(
            entries[CONTRACT.PACKAGE_PROVENANCE_ENTRY].decode("utf-8")
        )
        manifest["winui_configuration"] = "Debug"
        entries[CONTRACT.PACKAGE_PROVENANCE_ENTRY] = (
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "WinUI configuration mismatch" in result.stderr


def test_clean_release_rejects_dirty_provenance() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_dirty_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root, dirty=True)
        result = _run(
            root,
            zip_path,
            build_dir,
            winui_dir,
            require_clean=True,
        )
        assert result.returncode == 1
        assert "release build provenance is dirty" in result.stderr


def test_zip_must_match_current_build() -> None:
    with tempfile.TemporaryDirectory(prefix="skydiag_release_zip_stale_") as td:
        root = Path(td)
        zip_path, build_dir, winui_dir = _make_fixture(root)
        filename, _ = CONTRACT.NATIVE_BUILD_ZIP_MAPPINGS[0]
        _write(build_dir / "bin" / filename, _minimal_pe(marker=b"rebuilt"))
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 1
        assert "build provenance artifact mismatch" in result.stderr


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
        _write_build_provenance(root, build_dir, winui_dir)
        with zipfile.ZipFile(zip_path, "r") as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        _refresh_package_provenance(root, build_dir, winui_dir, entries)
        _write_zip(zip_path, entries)
        result = _run(root, zip_path, build_dir, winui_dir)
        assert result.returncode == 0, result.stderr


def main() -> int:
    test_valid_release_zip_passes()
    test_pdb_entry_fails()
    test_matching_prerelease_suffix_passes()
    test_non_x64_entry_fails()
    test_all_pe_entries_include_unmapped_native_files()
    test_unmapped_native_x86_pe_fails()
    test_32bit_required_managed_pe_fails()
    test_plain_arm64_pe_fails_without_arm64x_metadata()
    test_versioned_filename_mismatch_fails()
    test_binary_version_mismatch_fails()
    test_skse_metadata_version_mismatch_fails()
    test_package_must_bind_exact_build_manifests()
    test_all_winui_build_files_are_bound_to_zip()
    test_build_provenance_requires_exact_artifact_coverage()
    test_ini_bytes_are_bound_to_source()
    test_static_data_bytes_are_bound_to_source()
    test_package_winui_configuration_is_bound()
    test_clean_release_rejects_dirty_provenance()
    test_zip_must_match_current_build()
    test_matching_vcpkg_version_passes()
    test_mismatched_vcpkg_version_fails()
    test_absent_vcpkg_version_is_allowed()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
