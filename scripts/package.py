#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
import zipfile
from pathlib import Path


def _timestamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S", time.localtime())


def _read_version(root: Path) -> str:
    """Read project version from CMakeLists.txt."""
    import re
    cml = root / "CMakeLists.txt"
    if cml.is_file():
        m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", cml.read_text())
        if m:
            return m.group(1)
    return _timestamp()  # fallback


def _find_artifact(build_dir: Path, bin_dir: Path | None, filename: str) -> Path | None:
    candidates: list[Path] = []

    if bin_dir:
        p = bin_dir / filename
        if p.is_file():
            return p

    for p in [
        build_dir / "bin" / filename,
        build_dir / filename,
        build_dir / "bin" / "Release" / filename,
        build_dir / "bin" / "RelWithDebInfo" / filename,
        build_dir / "bin" / "Debug" / filename,
    ]:
        if p.is_file():
            return p

    # Fallback: find latest match (can be expensive on huge build dirs).
    for p in build_dir.rglob(filename):
        if p.is_file():
            candidates.append(p)

    if not candidates:
        return None

    return max(candidates, key=lambda x: x.stat().st_mtime)


def _zip_dir(src_dir: Path, out_zip: Path) -> None:
    out_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in src_dir.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(src_dir).as_posix()
            zf.write(path, rel)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Package SkyrimDiag as an MO2-friendly zip.")
    parser.add_argument("--build-dir", default="build", help="CMake build directory (default: build)")
    parser.add_argument("--bin-dir", default="", help="Override binary output directory (optional)")
    parser.add_argument(
        "--winui-dir",
        default="build-winui",
        help="WinUI publish directory (default: build-winui)",
    )
    parser.add_argument("--out", default="", help="Output zip path (default: dist/SkyrimDiag_<timestamp>.zip)")
    parser.add_argument(
        "--no-pdb",
        action="store_true",
        help="Do not include PDB files even if present",
    )
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    build_dir = (root / args.build_dir).resolve()
    bin_dir = (root / args.bin_dir).resolve() if args.bin_dir else None
    winui_dir = (root / args.winui_dir).resolve()

    if not build_dir.exists():
        print(f"ERROR: build dir not found: {build_dir}", file=sys.stderr)
        return 2

    plugin_dll = _find_artifact(build_dir, bin_dir, "SkyrimDiag.dll")
    helper_exe = _find_artifact(build_dir, bin_dir, "SkyrimDiagHelper.exe")
    native_dll = _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpToolNative.dll")
    cli_exe = _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpToolCli.exe")

    if not plugin_dll:
        print("ERROR: could not find SkyrimDiag.dll. Build the project first.", file=sys.stderr)
        return 3
    if not helper_exe:
        print("ERROR: could not find SkyrimDiagHelper.exe. Build the project first.", file=sys.stderr)
        return 3
    if not native_dll:
        print("ERROR: could not find SkyrimDiagDumpToolNative.dll. Build the project first.", file=sys.stderr)
        return 3
    if not cli_exe:
        print("ERROR: could not find SkyrimDiagDumpToolCli.exe. Build the project first.", file=sys.stderr)
        return 3

    plugin_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiag.pdb")
    helper_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiagHelper.pdb")
    native_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpToolNative.pdb")
    cli_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpToolCli.pdb")

    winui_exe = None
    winui_publish_dir = None
    if winui_dir.exists():
        winui_exe = _find_artifact(winui_dir, None, "SkyrimDiagDumpToolWinUI.exe")
        if winui_exe:
            winui_publish_dir = winui_exe.parent
    if not winui_exe or not winui_publish_dir:
        print(f"ERROR: could not find SkyrimDiagDumpToolWinUI.exe under {winui_dir}", file=sys.stderr)
        return 3
    required_winui_assets = [
        "SkyrimDiagDumpToolWinUI.pri",
        "App.xbf",
        "MainWindow.xbf",
    ]
    for rel in required_winui_assets:
        required_path = winui_publish_dir / rel
        if not required_path.is_file():
            print(
                f"ERROR: required WinUI asset missing from {winui_publish_dir}: {rel}",
                file=sys.stderr,
            )
            return 6

    ini_plugin = root / "dist" / "SkyrimDiag.ini"
    ini_helper = root / "dist" / "SkyrimDiagHelper.ini"
    data_root = root / "dump_tool" / "data"
    dump_tool_data_files = [
        "hook_frameworks.json",
        "crash_signatures.json",
        "address_db/skyrimse_functions.json",
    ]
    if not ini_plugin.is_file():
        print(f"ERROR: missing {ini_plugin}", file=sys.stderr)
        return 4
    if not ini_helper.is_file():
        print(f"ERROR: missing {ini_helper}", file=sys.stderr)
        return 4

    out_zip = Path(args.out) if args.out else root / "dist" / f"Tullius_ctd_loger_v{_read_version(root)}.zip"
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
            if not src.is_file():
                continue
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
        winui_plugins_dir.mkdir(parents=True, exist_ok=True)
        excluded_top_level_dirs = {"publish", "win-x64", "x64"}
        for item in winui_publish_dir.rglob("*"):
            if not item.is_file():
                continue
            if args.no_pdb and item.suffix.lower() == ".pdb":
                continue
            rel = item.relative_to(winui_publish_dir)
            if rel.parts and rel.parts[0].lower() in excluded_top_level_dirs:
                # Avoid packaging nested build/publish outputs that duplicate WinUI runtime files.
                continue
            dst = winui_plugins_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, dst)
            copied_winui += 1
        if copied_winui == 0:
            print(
                f"ERROR: WinUI publish folder found but no files copied: {winui_publish_dir}",
                file=sys.stderr,
            )
            return 5

        shutil.copy2(native_dll, winui_plugins_dir / "SkyrimDiagDumpToolNative.dll")
        for rel in dump_tool_data_files:
            src = data_root / rel
            if not src.is_file():
                continue
            dst = winui_plugins_dir / "data" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        if native_pdb and native_pdb.is_file():
            shutil.copy2(native_pdb, winui_plugins_dir / "SkyrimDiagDumpToolNative.pdb")

        _zip_dir(pkg_root, out_zip)

    print(f"Wrote: {out_zip}")
    print(f"- Plugin: {plugin_dll}")
    print(f"- Helper: {helper_exe}")
    print(f"- DumpToolCli: {cli_exe}")
    print(f"- DumpToolWinUI: {winui_exe}")
    print(f"- DumpToolNative: {native_dll}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
