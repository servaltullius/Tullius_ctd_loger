#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
import zipfile
from pathlib import Path

from release_contract import (
    NATIVE_BUILD_ZIP_MAPPINGS,
    REQUIRED_X64_PE_ZIP_ENTRIES,
    REQUIRED_ZIP_ENTRIES,
    WINUI_BUILD_ZIP_MAPPINGS,
    assert_version_sources_agree,
    find_build_artifact,
    find_winui_build_root,
    nested_winui_path_regex,
    release_zip_name,
)

IMAGE_FILE_MACHINE_AMD64 = 0x8664
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


def _pe_machine(blob: bytes, entry: str) -> int:
    if len(blob) < 0x40 or blob[:2] != b"MZ":
        raise VerificationError(f"not a PE image: {entry}")
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe_offset + 6 > len(blob) or blob[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise VerificationError(f"invalid PE header: {entry}")
    return struct.unpack_from("<H", blob, pe_offset + 4)[0]


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


def verify_release_zip(
    repo_root: Path,
    zip_path: Path,
    build_dir: Path,
    winui_dir: Path,
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
        raise VerificationError(f"WinUI publish root not found under: {winui_dir}")

    with zipfile.ZipFile(zip_path, "r") as archive:
        names = archive.namelist()
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

        for entry in REQUIRED_X64_PE_ZIP_ENTRIES:
            if entry not in name_set:
                raise VerificationError(f"required x64 PE entry missing: {entry}")
            machine = _pe_machine(archive.read(entry), entry)
            if machine != IMAGE_FILE_MACHINE_AMD64:
                raise VerificationError(
                    f"non-x64 PE entry: {entry} (machine=0x{machine:04x})"
                )
            print(f"  - x64 PE: {entry}")

        for filename, entry in NATIVE_BUILD_ZIP_MAPPINGS:
            source = find_build_artifact(build_dir, None, filename)
            if source is None:
                raise VerificationError(
                    f"current native build artifact not found: {filename} under {build_dir}"
                )
            _verify_source_hash(archive, source, entry)

        for filename, entry in WINUI_BUILD_ZIP_MAPPINGS:
            source = winui_build_root / filename
            if not source.is_file():
                raise VerificationError(f"current WinUI build artifact not found: {source}")
            _verify_source_hash(archive, source, entry)

    print(f"  - versioned filename: {zip_path.name}")
    print("  - no PDB entries")
    print("  - archive matches current native and WinUI builds")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify the version, architecture, and build freshness of a release zip."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--zip", required=True, dest="zip_path")
    parser.add_argument("--build-dir", default="build-win")
    parser.add_argument("--winui-dir", default="build-winui")
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    zip_path = _resolve(repo_root, args.zip_path)
    build_dir = _resolve(repo_root, args.build_dir)
    winui_dir = _resolve(repo_root, args.winui_dir)

    try:
        verify_release_zip(repo_root, zip_path, build_dir, winui_dir)
    except (OSError, ValueError, zipfile.BadZipFile, VerificationError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
