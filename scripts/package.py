#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

from release_contract import (
    BUILD_PROVENANCE_FILENAME,
    NATIVE_BUILD_ZIP_MAPPINGS,
    PACKAGE_PROVENANCE_ENTRY,
    PACKAGE_PROVENANCE_SCHEMA,
    REQUIRED_WINUI_ASSETS,
    WINUI_BUILD_ZIP_MAPPINGS,
    assert_version_sources_agree,
    find_winui_build_root,
    native_artifact_path,
    release_zip_name,
    sha256_path,
    source_state,
    validate_build_provenance,
    winui_artifact_is_packaged,
)


def _zip_dir(src_dir: Path, out_zip: Path) -> None:
    out_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(p for p in src_dir.rglob("*") if p.is_file()):
            rel = path.relative_to(src_dir).as_posix()
            info = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            zf.writestr(info, path.read_bytes())


def _collect_data_files(data_root: Path) -> list[Path]:
    if not data_root.is_dir():
        return []
    files = [p.relative_to(data_root) for p in data_root.rglob("*") if p.is_file()]
    files.sort()
    return files


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Package SkyrimDiag as an MO2-friendly zip."
    )
    parser.add_argument(
        "--build-dir", default="build", help="CMake build directory (default: build)"
    )
    parser.add_argument(
        "--bin-dir", default="", help="Override binary output directory (optional)"
    )
    parser.add_argument(
        "--config",
        default="RelWithDebInfo",
        help="Exact native CMake configuration to package (default: RelWithDebInfo)",
    )
    parser.add_argument(
        "--winui-dir",
        default="build-winui",
        help="WinUI publish directory (default: build-winui)",
    )
    parser.add_argument(
        "--out",
        default="",
        help="Output zip path (default: dist/Tullius_ctd_loger_v<version>.zip)",
    )
    parser.add_argument(
        "--no-pdb",
        action="store_true",
        help="Do not include PDB files even if present",
    )
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    try:
        version = assert_version_sources_agree(root)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    build_dir = (root / args.build_dir).resolve()
    bin_dir = (root / args.bin_dir).resolve() if args.bin_dir else None
    winui_dir = (root / args.winui_dir).resolve()

    if not build_dir.exists():
        print(f"ERROR: build dir not found: {build_dir}", file=sys.stderr)
        return 2

    try:
        native_artifacts = {
            filename: native_artifact_path(
                build_dir, args.config, filename, bin_dir=bin_dir
            )
            for filename, _ in NATIVE_BUILD_ZIP_MAPPINGS
        }
    except (FileNotFoundError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    plugin_dll = native_artifacts["SkyrimDiag.dll"]
    helper_exe = native_artifacts["SkyrimDiagHelper.exe"]
    native_dll = native_artifacts["SkyrimDiagDumpToolNative.dll"]
    cli_exe = native_artifacts["SkyrimDiagDumpToolCli.exe"]
    winui_launcher_exe = native_artifacts["SkyrimDiagDumpToolWinUI.exe"]

    plugin_pdb = (
        None
        if args.no_pdb
        else native_artifact_path(
            build_dir, args.config, "SkyrimDiag.pdb", bin_dir=bin_dir
        )
    )
    helper_pdb = (
        None
        if args.no_pdb
        else native_artifact_path(
            build_dir, args.config, "SkyrimDiagHelper.pdb", bin_dir=bin_dir
        )
    )
    native_pdb = (
        None
        if args.no_pdb
        else native_artifact_path(
            build_dir,
            args.config,
            "SkyrimDiagDumpToolNative.pdb",
            bin_dir=bin_dir,
        )
    )
    cli_pdb = (
        None
        if args.no_pdb
        else native_artifact_path(
            build_dir, args.config, "SkyrimDiagDumpToolCli.pdb", bin_dir=bin_dir
        )
    )

    winui_publish_dir = find_winui_build_root(winui_dir)
    if not winui_publish_dir:
        print(
            f"ERROR: could not find SkyrimDiagDumpToolWinUI.exe under {winui_dir}",
            file=sys.stderr,
        )
        return 3
    winui_exe = winui_publish_dir / "SkyrimDiagDumpToolWinUI.exe"
    for rel in REQUIRED_WINUI_ASSETS:
        required_path = winui_publish_dir / rel
        if not required_path.is_file():
            print(
                f"ERROR: required WinUI asset missing from {winui_publish_dir}: {rel}",
                file=sys.stderr,
            )
            return 6

    native_manifest = plugin_dll.parent / BUILD_PROVENANCE_FILENAME
    winui_manifest = winui_publish_dir / BUILD_PROVENANCE_FILENAME
    winui_artifacts = {
        path.relative_to(winui_publish_dir).as_posix(): path
        for path in winui_publish_dir.rglob("*")
        if path.is_file() and path.name != BUILD_PROVENANCE_FILENAME
    }
    try:
        native_provenance = validate_build_provenance(
            native_manifest,
            root,
            kind="native",
            configuration=args.config,
            artifacts=native_artifacts,
        )
        winui_provenance = validate_build_provenance(
            winui_manifest,
            root,
            kind="winui",
            configuration="Release",
            artifacts=winui_artifacts,
        )
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 7
    state = source_state(root)
    for label, provenance in (
        ("native", native_provenance),
        ("winui", winui_provenance),
    ):
        for field in ("git_commit", "git_dirty", "source_tree_sha256"):
            if provenance.get(field) != state[field]:
                print(
                    f"ERROR: {label} provenance source state changed during packaging "
                    f"for {field}",
                    file=sys.stderr,
                )
                return 7
    native_manifest_hash = sha256_path(native_manifest)
    winui_manifest_hash = sha256_path(winui_manifest)

    ini_plugin = root / "dist" / "SkyrimDiag.ini"
    ini_helper = root / "dist" / "SkyrimDiagHelper.ini"
    data_root = root / "dump_tool" / "data"
    dump_tool_data_files = _collect_data_files(data_root)
    if not ini_plugin.is_file():
        print(f"ERROR: missing {ini_plugin}", file=sys.stderr)
        return 4
    if not ini_helper.is_file():
        print(f"ERROR: missing {ini_helper}", file=sys.stderr)
        return 4
    if not dump_tool_data_files:
        print(
            f"ERROR: no dump tool data files found under {data_root}", file=sys.stderr
        )
        return 4

    out_zip = (
        Path(args.out)
        if args.out
        else root / "dist" / release_zip_name(f"v{version}")
    )
    if not out_zip.is_absolute():
        out_zip = (root / out_zip).resolve()

    with tempfile.TemporaryDirectory(prefix="skyrimdiag_pkg_") as td:
        pkg_root = Path(td)
        plugins_dir = pkg_root / "SKSE" / "Plugins"
        plugins_dir.mkdir(parents=True, exist_ok=True)

        shutil.copy2(plugin_dll, plugins_dir / "SkyrimDiag.dll")
        shutil.copy2(helper_exe, plugins_dir / "SkyrimDiagHelper.exe")
        shutil.copy2(cli_exe, plugins_dir / "SkyrimDiagDumpToolCli.exe")
        shutil.copy2(ini_plugin, plugins_dir / "SkyrimDiag.ini")
        shutil.copy2(ini_helper, plugins_dir / "SkyrimDiagHelper.ini")
        for rel in dump_tool_data_files:
            src = data_root / rel
            dst = plugins_dir / "data" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

        if plugin_pdb and plugin_pdb.is_file():
            shutil.copy2(plugin_pdb, plugins_dir / "SkyrimDiag.pdb")
        if helper_pdb and helper_pdb.is_file():
            shutil.copy2(helper_pdb, plugins_dir / "SkyrimDiagHelper.pdb")
        if cli_pdb and cli_pdb.is_file():
            shutil.copy2(cli_pdb, plugins_dir / "SkyrimDiagDumpToolCli.pdb")

        copied_winui = 0
        winui_plugins_dir = plugins_dir / "SkyrimDiagWinUI"
        winui_app_dir = winui_plugins_dir / "app"
        winui_plugins_dir.mkdir(parents=True, exist_ok=True)
        winui_app_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(winui_launcher_exe, winui_plugins_dir / "SkyrimDiagDumpToolWinUI.exe")
        for item in winui_publish_dir.rglob("*"):
            if not item.is_file():
                continue
            rel = item.relative_to(winui_publish_dir)
            if not winui_artifact_is_packaged(
                rel.as_posix(), include_pdb=not args.no_pdb
            ):
                continue
            dst = winui_app_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, dst)
            copied_winui += 1
        if copied_winui == 0:
            print(
                f"ERROR: WinUI publish folder found but no files copied: {winui_publish_dir}",
                file=sys.stderr,
            )
            return 5

        shutil.copy2(native_dll, winui_app_dir / "SkyrimDiagDumpToolNative.dll")
        for rel in dump_tool_data_files:
            src = data_root / rel
            dst = winui_app_dir / "data" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        if native_pdb and native_pdb.is_file():
            shutil.copy2(native_pdb, winui_app_dir / "SkyrimDiagDumpToolNative.pdb")

        final_state = source_state(root)
        if final_state != state:
            print(
                "ERROR: source state changed while assembling the package",
                file=sys.stderr,
            )
            return 7
        if (
            sha256_path(native_manifest) != native_manifest_hash
            or sha256_path(winui_manifest) != winui_manifest_hash
        ):
            print(
                "ERROR: build provenance changed while assembling the package",
                file=sys.stderr,
            )
            return 7
        package_artifacts = {
            path.relative_to(pkg_root).as_posix(): sha256_path(path)
            for path in sorted(p for p in pkg_root.rglob("*") if p.is_file())
        }
        package_manifest = {
            "schema": PACKAGE_PROVENANCE_SCHEMA,
            "version": version,
            **state,
            "native_configuration": args.config,
            "winui_configuration": "Release",
            "native_build_provenance_sha256": native_manifest_hash,
            "winui_build_provenance_sha256": winui_manifest_hash,
            "artifacts": package_artifacts,
        }
        package_manifest_path = pkg_root / Path(PACKAGE_PROVENANCE_ENTRY)
        package_manifest_path.parent.mkdir(parents=True, exist_ok=True)
        package_manifest_path.write_text(
            json.dumps(package_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        out_zip.parent.mkdir(parents=True, exist_ok=True)
        fd, temp_name = tempfile.mkstemp(
            prefix=f".{out_zip.name}.",
            suffix=".tmp",
            dir=out_zip.parent,
        )
        os.close(fd)
        temp_zip = Path(temp_name)
        try:
            _zip_dir(pkg_root, temp_zip)
            os.replace(temp_zip, out_zip)
        finally:
            temp_zip.unlink(missing_ok=True)

    print(f"Wrote: {out_zip}")
    print(f"- Plugin: {plugin_dll}")
    print(f"- Helper: {helper_exe}")
    print(f"- DumpToolCli: {cli_exe}")
    print(f"- DumpToolWinUI launcher: {winui_launcher_exe}")
    print(f"- DumpToolWinUI app: {winui_exe}")
    print(f"- DumpToolNative: {native_dll}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
