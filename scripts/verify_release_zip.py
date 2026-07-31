#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import zipfile
from pathlib import Path, PurePosixPath

from release_contract import (
    BUILD_PROVENANCE_FILENAME,
    NATIVE_BUILD_ZIP_MAPPINGS,
    PACKAGE_PROVENANCE_ENTRY,
    PACKAGE_PROVENANCE_SCHEMA,
    PROJECT_VERSIONED_PE_ZIP_ENTRIES,
    REQUIRED_ZIP_ENTRIES,
    WINUI_BUILD_ZIP_MAPPINGS,
    assert_version_sources_agree,
    find_winui_build_root,
    native_artifact_path,
    nested_winui_path_regex,
    release_zip_name,
    source_state,
    validate_build_provenance,
    winui_artifact_is_packaged,
)

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_FILE_MACHINE_ARM64X = 0xA64E
COMIMAGE_FLAGS_ILONLY = 0x00000001
COMIMAGE_FLAGS_32BITREQUIRED = 0x00000002
COMIMAGE_FLAGS_NATIVE_ENTRYPOINT = 0x00000010
COMIMAGE_FLAGS_32BITPREFERRED = 0x00020000
MAX_RELEASE_ZIP_BYTES = 100 * 1024 * 1024


class VerificationError(RuntimeError):
    pass


def _resolve(root: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = root / path
    return path.resolve()


def _sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_zip_entry(archive: zipfile.ZipFile, entry: str) -> str:
    digest = hashlib.sha256()
    with archive.open(entry, "r") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _pe_architecture(blob: bytes, entry: str) -> tuple[int, int | None]:
    if len(blob) < 0x40 or blob[:2] != b"MZ":
        raise VerificationError(f"not a PE image: {entry}")
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe_offset + 24 > len(blob) or blob[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise VerificationError(f"invalid PE header: {entry}")
    machine = struct.unpack_from("<H", blob, pe_offset + 4)[0]
    section_count = struct.unpack_from("<H", blob, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    optional_end = optional_offset + optional_size
    if optional_end > len(blob):
        raise VerificationError(f"truncated PE optional header: {entry}")

    magic = struct.unpack_from("<H", blob, optional_offset)[0]
    if magic == 0x20B:
        directory_count_offset = optional_offset + 108
        data_directory_offset = optional_offset + 112
    elif magic == 0x10B:
        directory_count_offset = optional_offset + 92
        data_directory_offset = optional_offset + 96
    else:
        raise VerificationError(f"unsupported PE optional-header magic: {entry}")
    if directory_count_offset + 4 > optional_end:
        raise VerificationError(f"truncated PE data-directory count: {entry}")

    directory_count = struct.unpack_from("<I", blob, directory_count_offset)[0]
    clr_directory_index = 14
    if directory_count <= clr_directory_index:
        return machine, None
    clr_directory_offset = data_directory_offset + clr_directory_index * 8
    if clr_directory_offset + 8 > optional_end:
        raise VerificationError(f"truncated PE CLR data directory: {entry}")
    clr_rva, clr_size = struct.unpack_from("<II", blob, clr_directory_offset)
    if clr_rva == 0 and clr_size == 0:
        return machine, None
    if clr_rva == 0 or clr_size < 20:
        raise VerificationError(f"invalid PE CLR data directory: {entry}")

    size_of_headers = (
        struct.unpack_from("<I", blob, optional_offset + 60)[0]
        if optional_offset + 64 <= optional_end
        else 0
    )
    section_offset = optional_end
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        offset = section_offset + index * 40
        if offset + 40 > len(blob):
            raise VerificationError(f"truncated PE section table: {entry}")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", blob, offset + 8
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_to_offset(rva: int, size: int) -> int:
        if rva + size <= size_of_headers and rva + size <= len(blob):
            return rva
        for virtual_address, span, raw_offset, raw_size in sections:
            if virtual_address <= rva and rva + size <= virtual_address + span:
                relative = rva - virtual_address
                if relative + size <= raw_size and raw_offset + relative + size <= len(blob):
                    return raw_offset + relative
                break
        raise VerificationError(f"PE CLR header is not file-backed: {entry}")

    clr_offset = rva_to_offset(clr_rva, 20)
    clr_header_size = struct.unpack_from("<I", blob, clr_offset)[0]
    if clr_header_size < 20:
        raise VerificationError(f"invalid PE CLR header size: {entry}")
    clr_flags = struct.unpack_from("<I", blob, clr_offset + 16)[0]
    return machine, clr_flags


def _verify_pe_architecture(blob: bytes, entry: str) -> str:
    machine, clr_flags = _pe_architecture(blob, entry)
    if clr_flags is not None and clr_flags & (
        COMIMAGE_FLAGS_32BITREQUIRED | COMIMAGE_FLAGS_32BITPREFERRED
    ):
        raise VerificationError(
            f"non-x64 PE entry: {entry} "
            f"(machine=0x{machine:04x}, CLR flags=0x{clr_flags:08x})"
        )
    if machine == IMAGE_FILE_MACHINE_AMD64:
        return "x64 managed PE" if clr_flags is not None else "x64 native PE"
    if machine in {IMAGE_FILE_MACHINE_ARM64, IMAGE_FILE_MACHINE_ARM64X} and (
        _pe_has_arm64x_hybrid_metadata(blob, entry)
    ):
        return "x64-compatible Arm64X PE"
    if (
        machine == IMAGE_FILE_MACHINE_I386
        and clr_flags is not None
        and clr_flags & COMIMAGE_FLAGS_ILONLY
        and not clr_flags & COMIMAGE_FLAGS_NATIVE_ENTRYPOINT
    ):
        return "architecture-neutral managed PE"
    details = (
        f", CLR flags=0x{clr_flags:08x}" if clr_flags is not None else ""
    )
    raise VerificationError(
        f"non-x64 PE entry: {entry} (machine=0x{machine:04x}{details})"
    )


def _pe_fixed_versions(
    blob: bytes, entry: str
) -> tuple[tuple[int, int, int, int], tuple[int, int, int, int]]:
    signature = struct.pack("<I", 0xFEEF04BD)
    offset = blob.find(signature)
    while offset >= 0:
        if offset + 24 <= len(blob):
            structure_version = struct.unpack_from("<I", blob, offset + 4)[0]
            if structure_version == 0x00010000:
                file_ms, file_ls, product_ms, product_ls = struct.unpack_from(
                    "<IIII", blob, offset + 8
                )
                file_version = (
                    file_ms >> 16,
                    file_ms & 0xFFFF,
                    file_ls >> 16,
                    file_ls & 0xFFFF,
                )
                product_version = (
                    product_ms >> 16,
                    product_ms & 0xFFFF,
                    product_ls >> 16,
                    product_ls & 0xFFFF,
                )
                return file_version, product_version
        offset = blob.find(signature, offset + 1)
    raise VerificationError(f"VERSIONINFO fixed metadata missing: {entry}")


def _pe_rva_reader(blob: bytes, entry: str):
    if len(blob) < 0x40 or blob[:2] != b"MZ":
        raise VerificationError(f"not a PE image: {entry}")
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe_offset + 24 > len(blob) or blob[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise VerificationError(f"invalid PE header: {entry}")
    section_count = struct.unpack_from("<H", blob, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    if optional_offset + optional_size > len(blob):
        raise VerificationError(f"truncated PE optional header: {entry}")
    magic = struct.unpack_from("<H", blob, optional_offset)[0]
    if magic == 0x20B:
        data_directory_offset = optional_offset + 112
    elif magic == 0x10B:
        data_directory_offset = optional_offset + 96
    else:
        raise VerificationError(f"unsupported PE optional-header magic: {entry}")
    if data_directory_offset + 8 > optional_offset + optional_size:
        raise VerificationError(f"PE export directory missing: {entry}")
    export_rva, export_size = struct.unpack_from("<II", blob, data_directory_offset)

    section_offset = optional_offset + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        offset = section_offset + index * 40
        if offset + 40 > len(blob):
            raise VerificationError(f"truncated PE section table: {entry}")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", blob, offset + 8
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_to_offset(rva: int, size: int = 1) -> int:
        for virtual_address, span, raw_offset, raw_size in sections:
            if virtual_address <= rva and rva + size <= virtual_address + span:
                relative = rva - virtual_address
                if relative + size > raw_size:
                    break
                file_offset = raw_offset + relative
                if file_offset + size <= len(blob):
                    return file_offset
                break
        raise VerificationError(f"PE RVA 0x{rva:x} is not file-backed: {entry}")

    return export_rva, export_size, rva_to_offset


def _pe_has_arm64x_hybrid_metadata(blob: bytes, entry: str) -> bool:
    """Recognize ARM64X images that default to an ARM64 COFF machine value."""
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    optional_size = struct.unpack_from("<H", blob, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    optional_end = optional_offset + optional_size
    magic = struct.unpack_from("<H", blob, optional_offset)[0]
    if magic != 0x20B:
        return False

    directory_count_offset = optional_offset + 108
    data_directory_offset = optional_offset + 112
    if directory_count_offset + 4 > optional_end:
        return False
    directory_count = struct.unpack_from("<I", blob, directory_count_offset)[0]
    load_config_index = 10
    if directory_count <= load_config_index:
        return False
    load_directory_offset = data_directory_offset + load_config_index * 8
    if load_directory_offset + 8 > optional_end:
        return False
    load_rva, load_size = struct.unpack_from("<II", blob, load_directory_offset)

    # IMAGE_LOAD_CONFIG_DIRECTORY64::CHPEMetadataPointer ends at byte 208.
    chpe_pointer_end = 208
    if load_rva == 0 or load_size < chpe_pointer_end:
        return False
    _, _, rva_to_offset = _pe_rva_reader(blob, entry)
    load_offset = rva_to_offset(load_rva, chpe_pointer_end)
    declared_size = struct.unpack_from("<I", blob, load_offset)[0]
    if declared_size < chpe_pointer_end:
        return False
    chpe_metadata_va = struct.unpack_from("<Q", blob, load_offset + 200)[0]
    image_base = struct.unpack_from("<Q", blob, optional_offset + 24)[0]
    if image_base == 0 or chpe_metadata_va < image_base:
        return False
    metadata_rva = chpe_metadata_va - image_base
    if metadata_rva > 0xFFFFFFFF:
        return False
    metadata_offset = rva_to_offset(int(metadata_rva), 4)
    metadata_version = struct.unpack_from("<I", blob, metadata_offset)[0]
    return metadata_version != 0


def _read_c_string(blob: bytes, offset: int, entry: str) -> str:
    end = blob.find(b"\0", offset)
    if end < 0:
        raise VerificationError(f"unterminated PE export name: {entry}")
    return blob[offset:end].decode("ascii", errors="strict")


def _skse_plugin_packed_version(blob: bytes, entry: str) -> int:
    export_rva, export_size, rva_to_offset = _pe_rva_reader(blob, entry)
    if export_rva == 0 or export_size == 0:
        raise VerificationError(f"PE export directory missing: {entry}")
    export_offset = rva_to_offset(export_rva, 40)
    function_count, name_count, functions_rva, names_rva, ordinals_rva = struct.unpack_from(
        "<IIIII", blob, export_offset + 20
    )
    if function_count == 0 or name_count == 0 or name_count > 100000:
        raise VerificationError(f"invalid PE export counts: {entry}")
    functions_offset = rva_to_offset(functions_rva, function_count * 4)
    names_offset = rva_to_offset(names_rva, name_count * 4)
    ordinals_offset = rva_to_offset(ordinals_rva, name_count * 2)
    for index in range(name_count):
        name_rva = struct.unpack_from("<I", blob, names_offset + index * 4)[0]
        name = _read_c_string(blob, rva_to_offset(name_rva), entry)
        if name != "SKSEPlugin_Version":
            continue
        ordinal = struct.unpack_from("<H", blob, ordinals_offset + index * 2)[0]
        if ordinal >= function_count:
            raise VerificationError(f"invalid SKSEPlugin_Version export ordinal: {entry}")
        data_rva = struct.unpack_from("<I", blob, functions_offset + ordinal * 4)[0]
        data_offset = rva_to_offset(data_rva, 8)
        data_version, packed_version = struct.unpack_from("<II", blob, data_offset)
        if data_version != 1:
            raise VerificationError(
                f"unsupported SKSEPlugin_Version data version {data_version}: {entry}"
            )
        return packed_version
    raise VerificationError(f"SKSEPlugin_Version export missing: {entry}")


def _verify_source_hash(
    archive: zipfile.ZipFile,
    source: Path,
    zip_entry: str,
) -> None:
    source_hash = _sha256_path(source)
    packaged_hash = _sha256_zip_entry(archive, zip_entry)
    if source_hash != packaged_hash:
        raise VerificationError(
            f"packaged file differs from current build: {zip_entry} (source={source})"
        )
    print(f"  - current build match: {zip_entry}")


def _provenance_artifact_hashes(
    provenance: dict[str, object], label: str
) -> dict[str, str]:
    raw = provenance.get("artifacts")
    if not isinstance(raw, dict):
        raise VerificationError(f"{label} provenance artifacts must be an object")
    hashes: dict[str, str] = {}
    for name, value in raw.items():
        if not isinstance(name, str) or not isinstance(value, str):
            raise VerificationError(f"{label} provenance contains an invalid artifact hash")
        hashes[name] = value
    return hashes


def _verify_winui_build_bindings(
    archive: zipfile.ZipFile,
    name_set: set[str],
    provenance: dict[str, object],
) -> set[str]:
    app_prefix = "SKSE/Plugins/SkyrimDiagWinUI/app/"
    expected_entries: set[str] = set()
    for rel, expected_hash in _provenance_artifact_hashes(
        provenance, "WinUI build"
    ).items():
        if not winui_artifact_is_packaged(rel, include_pdb=False):
            continue
        normalized = PurePosixPath(rel.replace("\\", "/")).as_posix()
        entry = f"{app_prefix}{normalized}"
        expected_entries.add(entry)
        if entry not in name_set:
            raise VerificationError(
                f"WinUI build artifact missing from package: {entry}"
            )
        if _sha256_zip_entry(archive, entry) != expected_hash:
            raise VerificationError(
                f"WinUI build-to-package hash mismatch: {entry}"
            )
    return expected_entries


def _verify_static_source_bindings(
    archive: zipfile.ZipFile,
    name_set: set[str],
    repo_root: Path,
) -> set[str]:
    expected_entries: set[str] = set()
    ini_bindings = (
        (repo_root / "dist" / "SkyrimDiag.ini", "SKSE/Plugins/SkyrimDiag.ini"),
        (
            repo_root / "dist" / "SkyrimDiagHelper.ini",
            "SKSE/Plugins/SkyrimDiagHelper.ini",
        ),
    )
    for source, entry in ini_bindings:
        if not source.is_file():
            raise VerificationError(f"static package source missing: {source}")
        if entry not in name_set:
            raise VerificationError(f"static package entry missing: {entry}")
        _verify_source_hash(archive, source, entry)
        expected_entries.add(entry)

    data_root = repo_root / "dump_tool" / "data"
    data_files = sorted(
        path for path in data_root.rglob("*") if path.is_file()
    ) if data_root.is_dir() else []
    if not data_files:
        raise VerificationError(f"dump-tool data source is empty: {data_root}")
    relative_data = {
        path.relative_to(data_root).as_posix(): path for path in data_files
    }
    for prefix in (
        "SKSE/Plugins/data/",
        "SKSE/Plugins/SkyrimDiagWinUI/app/data/",
    ):
        expected_under_prefix = {f"{prefix}{rel}" for rel in relative_data}
        actual_under_prefix = {
            name
            for name in name_set
            if name.startswith(prefix) and not name.endswith("/")
        }
        if actual_under_prefix != expected_under_prefix:
            missing = sorted(expected_under_prefix - actual_under_prefix)
            extra = sorted(actual_under_prefix - expected_under_prefix)
            raise VerificationError(
                f"static data coverage mismatch under {prefix}: "
                f"missing={missing[:1]} extra={extra[:1]}"
            )
        for rel, source in relative_data.items():
            entry = f"{prefix}{rel}"
            _verify_source_hash(archive, source, entry)
        expected_entries.update(expected_under_prefix)
    return expected_entries


def verify_release_zip(
    repo_root: Path,
    zip_path: Path,
    build_dir: Path,
    winui_dir: Path,
    configuration: str,
    require_clean: bool,
) -> None:
    # Fails the release rather than shipping a build whose declared versions
    # disagree with each other.
    version = assert_version_sources_agree(repo_root)
    print(f"  - version sources agree: {version}")
    stable_name = release_zip_name(f"v{version}")
    versioned_stem = stable_name.removesuffix(".zip")
    filename_pattern = re.compile(
        rf"{re.escape(versioned_stem)}(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?\.zip"
    )
    if filename_pattern.fullmatch(zip_path.name) is None:
        raise VerificationError(
            f"release zip filename mismatch: expected {versioned_stem}.zip or a matching "
            f"prerelease suffix, got {zip_path.name}"
        )
    if not zip_path.is_file():
        raise VerificationError(f"release zip not found: {zip_path}")
    if zip_path.stat().st_size > MAX_RELEASE_ZIP_BYTES:
        raise VerificationError(
            f"release zip exceeds 100 MiB: {zip_path.stat().st_size} bytes"
        )

    winui_build_root = find_winui_build_root(winui_dir)
    if winui_build_root is None:
        raise VerificationError(f"exact WinUI publish root is incomplete: {winui_dir}")

    native_artifacts: dict[str, Path] = {}
    for filename, _ in NATIVE_BUILD_ZIP_MAPPINGS:
        try:
            native_artifacts[filename] = native_artifact_path(
                build_dir, configuration, filename
            )
        except (FileNotFoundError, ValueError) as exc:
            raise VerificationError(str(exc)) from exc
    winui_artifacts = {
        path.relative_to(winui_build_root).as_posix(): path
        for path in winui_build_root.rglob("*")
        if path.is_file() and path.name != BUILD_PROVENANCE_FILENAME
    }
    try:
        native_provenance = validate_build_provenance(
            native_artifacts["SkyrimDiag.dll"].parent / BUILD_PROVENANCE_FILENAME,
            repo_root,
            kind="native",
            configuration=configuration,
            artifacts=native_artifacts,
            require_clean=require_clean,
        )
        winui_provenance = validate_build_provenance(
            winui_build_root / BUILD_PROVENANCE_FILENAME,
            repo_root,
            kind="winui",
            configuration="Release",
            artifacts=winui_artifacts,
            require_clean=require_clean,
        )
    except (OSError, ValueError) as exc:
        raise VerificationError(str(exc)) from exc
    expected_state = source_state(repo_root)
    for label, provenance in (
        ("native", native_provenance),
        ("winui", winui_provenance),
    ):
        for field in ("git_commit", "git_dirty", "source_tree_sha256"):
            if provenance.get(field) != expected_state[field]:
                raise VerificationError(
                    f"{label} build provenance source state changed during verification "
                    f"for {field}"
                )

    with zipfile.ZipFile(zip_path, "r") as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise VerificationError("release zip contains duplicate entry names")
        name_set = set(names)

        missing = [entry for entry in REQUIRED_ZIP_ENTRIES if entry not in name_set]
        if missing:
            raise VerificationError(f"required zip entry missing: {missing[0]}")

        pdb_entries = [name for name in names if name.lower().endswith(".pdb")]
        if pdb_entries:
            raise VerificationError(f"PDB must not be shipped: {pdb_entries[0]}")

        nested_pattern = re.compile(nested_winui_path_regex())
        nested_entries = [name for name in names if nested_pattern.search(name)]
        if nested_entries:
            raise VerificationError(
                f"nested WinUI build output must not be shipped: {nested_entries[0]}"
            )

        try:
            package_provenance = json.loads(
                archive.read(PACKAGE_PROVENANCE_ENTRY).decode("utf-8")
            )
        except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise VerificationError(f"invalid package provenance: {exc}") from exc
        if (
            not isinstance(package_provenance, dict)
            or package_provenance.get("schema") != PACKAGE_PROVENANCE_SCHEMA
        ):
            raise VerificationError("unsupported package provenance schema")
        if package_provenance.get("version") != version:
            raise VerificationError("package provenance version mismatch")
        if package_provenance.get("native_configuration") != configuration:
            raise VerificationError("package provenance native configuration mismatch")
        if package_provenance.get("winui_configuration") != "Release":
            raise VerificationError("package provenance WinUI configuration mismatch")
        current_state = source_state(repo_root)
        if current_state != expected_state:
            raise VerificationError("source state changed during release verification")
        for field in ("git_commit", "git_dirty", "source_tree_sha256"):
            if package_provenance.get(field) != expected_state[field]:
                raise VerificationError(
                    f"package provenance source state mismatch for {field}"
                )
        if require_clean and bool(package_provenance.get("git_dirty")):
            raise VerificationError("release package provenance is dirty")

        native_manifest_path = (
            native_artifacts["SkyrimDiag.dll"].parent / BUILD_PROVENANCE_FILENAME
        )
        winui_manifest_path = winui_build_root / BUILD_PROVENANCE_FILENAME
        expected_manifest_hashes = {
            "native_build_provenance_sha256": _sha256_path(native_manifest_path),
            "winui_build_provenance_sha256": _sha256_path(winui_manifest_path),
        }
        for field, expected_hash in expected_manifest_hashes.items():
            if package_provenance.get(field) != expected_hash:
                raise VerificationError(
                    f"package provenance build manifest mismatch for {field}"
                )

        recorded_hashes = package_provenance.get("artifacts")
        if not isinstance(recorded_hashes, dict):
            raise VerificationError("package provenance artifacts must be an object")
        expected_recorded_names = name_set - {PACKAGE_PROVENANCE_ENTRY}
        if set(recorded_hashes) != expected_recorded_names:
            missing_record = sorted(expected_recorded_names - set(recorded_hashes))
            extra_record = sorted(set(recorded_hashes) - expected_recorded_names)
            raise VerificationError(
                "package provenance coverage mismatch: "
                f"missing={missing_record[:1]} extra={extra_record[:1]}"
            )
        for entry, expected_hash in recorded_hashes.items():
            if not isinstance(expected_hash, str):
                raise VerificationError(f"invalid package provenance hash: {entry}")
            if _sha256_zip_entry(archive, entry) != expected_hash:
                raise VerificationError(f"package provenance hash mismatch: {entry}")

        winui_build_entries = _verify_winui_build_bindings(
            archive, name_set, winui_provenance
        )
        static_entries = _verify_static_source_bindings(
            archive, name_set, repo_root
        )
        exact_expected_entries = {
            PACKAGE_PROVENANCE_ENTRY,
            *(entry for _, entry in NATIVE_BUILD_ZIP_MAPPINGS),
            *winui_build_entries,
            *static_entries,
        }
        actual_file_entries = {name for name in name_set if not name.endswith("/")}
        if actual_file_entries != exact_expected_entries:
            missing = sorted(exact_expected_entries - actual_file_entries)
            extra = sorted(actual_file_entries - exact_expected_entries)
            raise VerificationError(
                "release payload coverage mismatch: "
                f"missing={missing[:1]} extra={extra[:1]}"
            )

        pe_entries = sorted(
            entry
            for entry in actual_file_entries
            if PurePosixPath(entry).suffix.casefold() in {".dll", ".exe"}
        )
        if not pe_entries:
            raise VerificationError("release payload contains no PE .exe/.dll entries")
        for entry in pe_entries:
            classification = _verify_pe_architecture(archive.read(entry), entry)
            print(f"  - {classification}: {entry}")

        expected_version = tuple(int(part) for part in version.split(".")) + (0,)
        for entry in PROJECT_VERSIONED_PE_ZIP_ENTRIES:
            file_version, product_version = _pe_fixed_versions(
                archive.read(entry), entry
            )
            if file_version != expected_version or product_version != expected_version:
                raise VerificationError(
                    f"binary version mismatch: {entry} "
                    f"(file={file_version}, product={product_version}, "
                    f"expected={expected_version})"
                )
            print(f"  - binary version {version}: {entry}")

        expected_skse_version = (
            (expected_version[0] & 0xFF) << 24
            | (expected_version[1] & 0xFF) << 16
            | (expected_version[2] & 0xFFF) << 4
            | (expected_version[3] & 0xF)
        )
        plugin_entry = "SKSE/Plugins/SkyrimDiag.dll"
        actual_skse_version = _skse_plugin_packed_version(
            archive.read(plugin_entry), plugin_entry
        )
        if actual_skse_version != expected_skse_version:
            raise VerificationError(
                "SKSE plugin metadata version mismatch: "
                f"packed=0x{actual_skse_version:08x}, "
                f"expected=0x{expected_skse_version:08x}"
            )
        print(f"  - SKSE plugin metadata version {version}")

        for filename, entry in NATIVE_BUILD_ZIP_MAPPINGS:
            source = native_artifacts[filename]
            _verify_source_hash(archive, source, entry)

        for filename, entry in WINUI_BUILD_ZIP_MAPPINGS:
            source = winui_build_root / filename
            if not source.is_file():
                raise VerificationError(f"current WinUI build artifact not found: {source}")
            _verify_source_hash(archive, source, entry)

    print(f"  - versioned filename: {zip_path.name}")
    print("  - no PDB entries")
    print(
        "  - archive matches exact native/WinUI builds and commit-bound provenance"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify the version, architecture, and build freshness of a release zip."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--zip", required=True, dest="zip_path")
    parser.add_argument("--build-dir", default="build-win")
    parser.add_argument("--winui-dir", default="build-winui")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--require-clean", action="store_true")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    zip_path = _resolve(repo_root, args.zip_path)
    build_dir = _resolve(repo_root, args.build_dir)
    winui_dir = _resolve(repo_root, args.winui_dir)

    try:
        verify_release_zip(
            repo_root,
            zip_path,
            build_dir,
            winui_dir,
            args.config,
            args.require_clean,
        )
    except (
        OSError,
        UnicodeError,
        ValueError,
        zipfile.BadZipFile,
        VerificationError,
    ) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
