#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

REQUIRED_WINUI_ASSETS = (
    "SkyrimDiagDumpToolWinUI.pri",
    "SkyrimDiagDumpToolWinUI.runtimeconfig.json",
    "SkyrimDiagDumpToolWinUI.deps.json",
    "App.xbf",
    "MainWindow.xbf",
)

REQUIRED_WINUI_BUILD_OUTPUTS = (
    "SkyrimDiagDumpToolWinUI.exe",
    *REQUIRED_WINUI_ASSETS,
)

REQUIRED_ZIP_ENTRIES = (
    "SKSE/Plugins/SkyrimDiagDumpToolCli.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.pri",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.runtimeconfig.json",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.deps.json",
    "SKSE/Plugins/SkyrimDiagWinUI/App.xbf",
    "SKSE/Plugins/SkyrimDiagWinUI/MainWindow.xbf",
)

EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset({"publish", "win-x64", "x64"})


def release_zip_name(tag_or_version: str) -> str:
    normalized = tag_or_version.strip()
    if not normalized:
        raise ValueError("tag_or_version must not be empty")
    return f"Tullius_ctd_loger_{normalized}.zip"


def release_zip_glob() -> str:
    return "Tullius_ctd_loger_v*.zip"


def nested_winui_path_regex() -> str:
    parts = "|".join(sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS))
    return rf"^SKSE/Plugins/SkyrimDiagWinUI/({parts})/"


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
