#!/usr/bin/env python3
from __future__ import annotations

import re
from pathlib import Path

REQUIRED_WINUI_RUNTIME_ASSETS = (
    "Microsoft.WindowsAppRuntime.Bootstrap.dll",
    "Microsoft.WindowsAppRuntime.dll",
    "Microsoft.WindowsAppRuntime.pri",
    "Microsoft.ui.xaml.dll",
    "Microsoft.UI.pri",
    "CoreMessagingXP.dll",
)

REQUIRED_WINUI_ASSETS = (
    "SkyrimDiagDumpToolWinUI.pri",
    "SkyrimDiagDumpToolWinUI.runtimeconfig.json",
    "SkyrimDiagDumpToolWinUI.deps.json",
    "App.xbf",
    "MainWindow.xbf",
    *REQUIRED_WINUI_RUNTIME_ASSETS,
)

REQUIRED_WINUI_BUILD_OUTPUTS = (
    "SkyrimDiagDumpToolWinUI.exe",
    *REQUIRED_WINUI_ASSETS,
)

REQUIRED_ZIP_ENTRIES = (
    "SKSE/Plugins/SkyrimDiagDumpToolCli.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolNative.dll",
    *(f"SKSE/Plugins/SkyrimDiagWinUI/app/{item}" for item in REQUIRED_WINUI_ASSETS),
)

NATIVE_BUILD_ZIP_MAPPINGS = (
    ("SkyrimDiag.dll", "SKSE/Plugins/SkyrimDiag.dll"),
    ("SkyrimDiagHelper.exe", "SKSE/Plugins/SkyrimDiagHelper.exe"),
    ("SkyrimDiagDumpToolCli.exe", "SKSE/Plugins/SkyrimDiagDumpToolCli.exe"),
    (
        "SkyrimDiagDumpToolWinUI.exe",
        "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe",
    ),
    (
        "SkyrimDiagDumpToolNative.dll",
        "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolNative.dll",
    ),
)

WINUI_BUILD_ZIP_MAPPINGS = (
    (
        "SkyrimDiagDumpToolWinUI.exe",
        "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.exe",
    ),
    *(
        (item, f"SKSE/Plugins/SkyrimDiagWinUI/app/{item}")
        for item in REQUIRED_WINUI_RUNTIME_ASSETS
    ),
)

REQUIRED_X64_PE_ZIP_ENTRIES = tuple(
    entry
    for _, entry in (*NATIVE_BUILD_ZIP_MAPPINGS, *WINUI_BUILD_ZIP_MAPPINGS)
    if Path(entry).suffix.lower() in {".dll", ".exe"}
)

EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset({"publish", "win-x64", "x64"})


def project_version(root: str | Path) -> str:
    cmake_lists = Path(root) / "CMakeLists.txt"
    text = cmake_lists.read_text(encoding="utf-8")
    project_match = re.search(r"\bproject\s*\((.*?)\)", text, re.IGNORECASE | re.DOTALL)
    version_match = (
        re.search(r"\bVERSION\s+(\d+\.\d+\.\d+)\b", project_match.group(1), re.IGNORECASE)
        if project_match
        else None
    )
    if version_match is None:
        raise ValueError(f"project VERSION not found in {cmake_lists}")
    return version_match.group(1)


def release_zip_name(tag_or_version: str) -> str:
    normalized = tag_or_version.strip()
    if not normalized:
        raise ValueError("tag_or_version must not be empty")
    return f"Tullius_ctd_loger_{normalized}.zip"


def release_zip_glob() -> str:
    return "Tullius_ctd_loger_v*.zip"


def nested_winui_path_regex() -> str:
    parts = "|".join(sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS))
    return rf"^SKSE/Plugins/SkyrimDiagWinUI/(app/)?({parts})/"


def find_build_artifact(
    build_dir: str | Path,
    bin_dir: str | Path | None,
    filename: str,
) -> Path | None:
    build_path = Path(build_dir)
    bin_path = Path(bin_dir) if bin_dir else None
    candidates: list[Path] = []

    if bin_path:
        path = bin_path / filename
        if path.is_file():
            return path

    for path in (
        build_path / "bin" / filename,
        build_path / filename,
        build_path / "bin" / "Release" / filename,
        build_path / "bin" / "RelWithDebInfo" / filename,
        build_path / "bin" / "Debug" / filename,
    ):
        if path.is_file():
            return path

    for path in build_path.rglob(filename):
        if path.is_file():
            candidates.append(path)

    if not candidates:
        return None
    return max(candidates, key=lambda item: item.stat().st_mtime_ns)


def find_winui_build_root(root: str | Path) -> Path | None:
    root_path = Path(root)
    if not root_path.exists():
        return None

    def is_valid_publish_dir(path: Path) -> bool:
        if not path.is_dir():
            return False
        return all((path / item).is_file() for item in REQUIRED_WINUI_BUILD_OUTPUTS)

    if is_valid_publish_dir(root_path):
        return root_path

    candidates = []
    for exe in root_path.rglob("SkyrimDiagDumpToolWinUI.exe"):
        parent = exe.parent
        if is_valid_publish_dir(parent):
            candidates.append(parent)

    if not candidates:
        return None

    candidates.sort(key=lambda p: (len(p.parts), -p.stat().st_mtime_ns))
    return candidates[0]
