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

    if not build_dir.exists():
        print(f"ERROR: build dir not found: {build_dir}", file=sys.stderr)
        return 2

    plugin_dll = _find_artifact(build_dir, bin_dir, "SkyrimDiag.dll")
    helper_exe = _find_artifact(build_dir, bin_dir, "SkyrimDiagHelper.exe")
    dump_tool_exe = _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpTool.exe")

    if not plugin_dll:
        print("ERROR: could not find SkyrimDiag.dll. Build the project first.", file=sys.stderr)
        return 3
    if not helper_exe:
        print("ERROR: could not find SkyrimDiagHelper.exe. Build the project first.", file=sys.stderr)
        return 3
    if not dump_tool_exe:
        print("ERROR: could not find SkyrimDiagDumpTool.exe. Build the project first.", file=sys.stderr)
        return 3

    plugin_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiag.pdb")
    helper_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiagHelper.pdb")
    dump_tool_pdb = None if args.no_pdb else _find_artifact(build_dir, bin_dir, "SkyrimDiagDumpTool.pdb")

    ini_plugin = root / "dist" / "SkyrimDiag.ini"
    ini_helper = root / "dist" / "SkyrimDiagHelper.ini"
    ini_dump_tool = root / "dist" / "SkyrimDiagDumpTool.ini"
    if not ini_plugin.is_file():
        print(f"ERROR: missing {ini_plugin}", file=sys.stderr)
        return 4
    if not ini_helper.is_file():
        print(f"ERROR: missing {ini_helper}", file=sys.stderr)
        return 4
    if not ini_dump_tool.is_file():
        print(f"ERROR: missing {ini_dump_tool}", file=sys.stderr)
        return 4

    out_zip = Path(args.out) if args.out else root / "dist" / f"SkyrimDiag_{_timestamp()}.zip"
    if not out_zip.is_absolute():
        out_zip = (root / out_zip).resolve()

    with tempfile.TemporaryDirectory(prefix="skyrimdiag_pkg_") as td:
        pkg_root = Path(td)
        plugins_dir = pkg_root / "SKSE" / "Plugins"
        plugins_dir.mkdir(parents=True, exist_ok=True)

        shutil.copy2(plugin_dll, plugins_dir / "SkyrimDiag.dll")
        shutil.copy2(helper_exe, plugins_dir / "SkyrimDiagHelper.exe")
        shutil.copy2(dump_tool_exe, plugins_dir / "SkyrimDiagDumpTool.exe")
        shutil.copy2(ini_plugin, plugins_dir / "SkyrimDiag.ini")
        shutil.copy2(ini_helper, plugins_dir / "SkyrimDiagHelper.ini")
        shutil.copy2(ini_dump_tool, plugins_dir / "SkyrimDiagDumpTool.ini")

        if plugin_pdb and plugin_pdb.is_file():
            shutil.copy2(plugin_pdb, plugins_dir / "SkyrimDiag.pdb")
        if helper_pdb and helper_pdb.is_file():
            shutil.copy2(helper_pdb, plugins_dir / "SkyrimDiagHelper.pdb")
        if dump_tool_pdb and dump_tool_pdb.is_file():
            shutil.copy2(dump_tool_pdb, plugins_dir / "SkyrimDiagDumpTool.pdb")

        _zip_dir(pkg_root, out_zip)

    print(f"Wrote: {out_zip}")
    print(f"- Plugin: {plugin_dll}")
    print(f"- Helper: {helper_exe}")
    print(f"- DumpTool: {dump_tool_exe}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
