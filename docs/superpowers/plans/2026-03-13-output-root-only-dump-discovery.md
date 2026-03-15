# Output-Root-Only Dump Discovery Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** dump discovery 자동 탐색을 generic crash dump가 아니라 툴리우스 output 위치 중심으로 바꾸고, UI 용어도 이에 맞게 정리한다.

**Architecture:** `DumpDiscoveryService`가 helper config + 설치 위치를 읽어 자동 output roots를 계산하고, WinUI start screen은 이 roots만 자동 스캔한다. generic `.dmp`는 `직접 선택` 예외 경로로만 남긴다.

**Tech Stack:** WinUI 3, C#, source-guard tests, existing Linux/Windows verification scripts

---

## Chunk 1: Output-Root Contract

### Task 1: Lock the new discovery contract with failing source-guard tests

**Files:**
- Modify: `tests/winui_xaml_tests.cpp`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add expectations that:
- `DumpDiscoveryService.cs` no longer references `CrashDumps`
- the service reads `SkyrimDiagHelper.ini` and `OutputDir`
- the service contains MO2 `overwrite` inference
- user-facing text uses `덤프 출력 위치`

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: FAIL because discovery is still generic-search based

## Chunk 2: Automatic Output Root Inference

### Task 2: Teach discovery service to infer helper output roots

**Files:**
- Modify: `dump_tool_winui/DumpDiscoveryService.cs`

- [ ] **Step 1: Write minimal implementation**

Implement:
- helper ini resolution from the packaged WinUI location
- `OutputDir=` parsing
- relative `OutputDir` resolution against helper exe dir
- MO2 overwrite inference when `OutputDir` is blank
- removal of generic `CrashDumps` fallback

- [ ] **Step 2: Run narrow verification**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: PASS

## Chunk 3: Output-Location UX Copy

### Task 3: Rename dump-search UX to output-location UX

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`

- [ ] **Step 1: Write minimal implementation**

Update:
- panel title/hint/empty-state strings
- discovery status text
- source labels shown for auto/registered/learned roots

- [ ] **Step 2: Run narrow verification**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: PASS

## Chunk 4: Full Verification

### Task 4: Validate Linux and Windows paths

**Files:**
- Verify only

- [ ] **Step 1: Run Linux verification**

Run: `ctest --test-dir build-linux-test --output-on-failure`
Expected: PASS

- [ ] **Step 2: Run Windows builds**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```
Expected: PASS

- [ ] **Step 3: Run WinUI smoke**

Launch `build-winui/SkyrimDiagDumpToolWinUI.exe`
Expected: process stays alive and start screen still opens normally
