# WinUI Full Replacement Plan (Executed)

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove runtime dependency on `SkyrimDiagDumpTool.exe` and ship WinUI + native analyzer DLL as the only viewer/analyzer path.

**Architecture:** Introduce a native C ABI bridge DLL (`SkyrimDiagDumpToolNative.dll`) over existing analyzer core (`AnalyzeDump` + `WriteOutputs`) and call it from WinUI via P/Invoke. Keep helper launching WinUI executable for both headless and auto-open flows. Package only WinUI runtime + native analyzer DLL.

**Tech Stack:** C++20/CMake (native bridge), WinUI 3/.NET 8 (P/Invoke), Python packaging script.

### Task 1: Native Bridge DLL
- Add exported `SkyrimDiagAnalyzeDumpW` function.
- Build `SkyrimDiagDumpToolNative.dll` in CMake.

### Task 2: WinUI Direct Native Invocation
- Replace process delegation to legacy EXE with P/Invoke bridge.
- Keep CLI compatibility flags (`--headless`, `--lang`, `--simple-ui`, `--advanced-ui`).

### Task 3: WinUI Built-in Advanced Diagnostics
- Add built-in advanced sections in WinUI:
  - callstack, evidence, resources, events, WCT JSON, report text.
- Remove "open legacy advanced viewer" action.

### Task 4: Helper and Packaging Replacement
- Remove helper fallback to legacy EXE.
- Package WinUI + native DLL only.
- Stop packaging legacy viewer EXE/INI.

### Task 5: Documentation + Verification
- Update README/changelog/checklist to full-replacement wording.
- Verify:
  - `dotnet build dump_tool_winui/...`
  - `ctest --test-dir build-linux --output-on-failure`
  - Windows `scripts/build-win.cmd`
  - Windows `scripts/build-winui.cmd`
  - Windows `scripts/package.py ...`
