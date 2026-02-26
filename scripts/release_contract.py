#!/usr/bin/env python3
from __future__ import annotations

REQUIRED_WINUI_ASSETS = (
    "SkyrimDiagDumpToolWinUI.pri",
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
    "SKSE/Plugins/SkyrimDiagWinUI/App.xbf",
    "SKSE/Plugins/SkyrimDiagWinUI/MainWindow.xbf",
)

EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset({"publish", "win-x64", "x64"})


def nested_winui_path_regex() -> str:
    parts = "|".join(sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS))
    return rf"^SKSE/Plugins/SkyrimDiagWinUI/({parts})/"
