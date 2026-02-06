# WinUI 3 Modernization Phase 1 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a modern WinUI 3 beginner-first UI that reuses the current C++ dump analyzer pipeline without breaking existing helper and packaging flows.

**Architecture:** Keep `SkyrimDiagDumpTool.exe` (C++ analyzer/viewer) as the stable analysis engine for now, and introduce `SkyrimDiagDumpToolWinUI.exe` (WinUI 3, unpackaged) as a modern shell. The WinUI app executes the legacy tool in `--headless` mode, then renders `*_SkyrimDiagSummary.json` with beginner-friendly UX. Helper and packaging are updated to prefer WinUI but automatically fall back to legacy.

**Tech Stack:** Windows App SDK (WinUI 3, stable channel), .NET 8, existing C++ dump tool + JSON outputs, Python packaging script.

### Task 1: Add WinUI 3 App Skeleton

**Files:**
- Create: `dump_tool_winui/SkyrimDiagDumpToolWinUI.csproj`
- Create: `dump_tool_winui/App.xaml`
- Create: `dump_tool_winui/App.xaml.cs`
- Create: `dump_tool_winui/MainWindow.xaml`
- Create: `dump_tool_winui/MainWindow.xaml.cs`
- Create: `dump_tool_winui/Properties/launchSettings.json`

**Step 1: Create an unpackaged WinUI 3 project definition**

Include:
- `<TargetFramework>net8.0-windows10.0.19041.0</TargetFramework>`
- `<UseWinUI>true</UseWinUI>`
- `<WindowsPackageType>None</WindowsPackageType>`

**Step 2: Add beginner-first main UI**

Add:
- Dump path picker
- Analyze button
- Summary card
- Suspect list
- Recommendations list
- Buttons for report folder and advanced viewer

**Step 3: Add async analysis orchestration**

Run:
- `SkyrimDiagDumpTool.exe <dump> --out-dir <dir> --headless [--lang]`

Then parse:
- `<stem>_SkyrimDiagSummary.json`

### Task 2: Keep CLI Compatibility for Helper

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml.cs`

**Step 1: Parse compatible CLI flags**

Support:
- `<dumpPath>`
- `--out-dir`
- `--headless`
- `--lang`
- `--simple-ui` / `--advanced-ui`

**Step 2: Implement headless-only mode**

If `--headless`:
- Execute legacy analyzer
- Return process exit code
- Do not show full interactive UI

### Task 3: Integrate Helper Fallback

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `helper/src/main.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`

**Step 1: Prefer WinUI executable by default**

Set default `DumpToolExe` to `SkyrimDiagDumpToolWinUI.exe`.

**Step 2: Add robust fallback**

If configured executable is missing:
- try `SkyrimDiagDumpToolWinUI.exe`
- then `SkyrimDiagDumpTool.exe`

### Task 4: Integrate Packaging and Build Guidance

**Files:**
- Modify: `scripts/package.py`
- Create: `scripts/build-winui.cmd`
- Modify: `README.md`

**Step 1: Extend package script**

Package WinUI output when available (without breaking legacy-only packaging).

**Step 2: Add WinUI build helper**

Add Windows script to publish WinUI output to predictable directory.

**Step 3: Update docs**

Document:
- WinUI build/publish command
- helper default/fallback behavior
- optional runtime prerequisites

### Task 5: Verify and Record Results

**Files:**
- Modify: `CHANGELOG.md` (if behavior/user workflow changes materially)

**Step 1: Run existing tests**

Run:
- `ctest --test-dir build-linux --output-on-failure`

**Step 2: Validate packaging script still works**

Run package script help or dry invocation and verify argument handling.

**Step 3: Validate WinUI build command on Windows toolchain**

Run publish command and confirm produced executable path.
