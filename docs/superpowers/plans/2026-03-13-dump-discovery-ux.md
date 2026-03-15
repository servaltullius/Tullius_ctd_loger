# Dump Discovery UX Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 덤프툴 시작 화면을 `최근 발견된 덤프` 중심으로 바꾸고, dump 검색 위치 등록과 자동 스캔을 통해 파일 찾기 마찰을 줄인다.

**Architecture:** 시작 화면은 `Dump Intake`의 입력형 구조를 유지하되, 그 위에 `recent dump discovery` 레이어를 추가한다. 검색 모델은 `학습 경로 / 등록 경로 / 자동 경로`를 결합한 작은 discovery 서비스로 분리하고, UI는 이 결과를 리스트와 empty state로 소비한다.

**Tech Stack:** WinUI 3, C#, existing ViewModel/XAML patterns, source-guard/UI tests

---

## Chunk 1: Discovery Contract

### Task 1: Define search-source and recent-dump contract

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add guard expectations for:
- recent dump list section in the analyze/start panel
- `폴더 관리` entry point
- empty state text for missing dumps

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: FAIL because the start panel does not expose the new discovery-first structure yet

## Chunk 2: Search Model

### Task 2: Add dump discovery sources and persistence hooks

**Files:**
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Create or Modify: nearest existing settings/persistence file used for WinUI preferences
- Test: add/update C#-level unit/source guard as appropriate

- [ ] **Step 1: Write the failing test**

Cover:
- multiple registered search roots
- learned paths promoted after successful open/analyze
- recent dump ordering by last write time

- [ ] **Step 2: Run test to verify it fails**

Run the narrowest relevant test target
Expected: FAIL because discovery state does not exist yet

- [ ] **Step 3: Write minimal implementation**

Implement:
- `registered search roots`
- `learned search roots`
- `automatic roots`
- recursive `.dmp` search limited to known roots

- [ ] **Step 4: Run tests to verify they pass**

Run the same narrow tests
Expected: PASS

## Chunk 3: Start Screen UX

### Task 3: Rework analyze/start screen around recent dumps

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Write the failing test**

Require:
- `최근 발견된 덤프` section
- `다시 스캔`, `폴더 관리`, `직접 선택` actions
- direct empty-state guidance mentioning `MO2 overwrite`

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: FAIL

- [ ] **Step 3: Write minimal implementation**

Implement:
- recent dump cards/list
- empty state
- move `Browse dump` to secondary action

- [ ] **Step 4: Run tests to verify it passes**

Run: `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
Expected: PASS

## Chunk 4: Folder Management UX

### Task 4: Add dump search location management

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: relevant WinUI/source-guard tests

- [ ] **Step 1: Write the failing test**

Require:
- multiple search locations
- add/remove UI
- wording uses `덤프 검색 위치`

- [ ] **Step 2: Run test to verify it fails**

Run narrow test target
Expected: FAIL

- [ ] **Step 3: Write minimal implementation**

Implement folder management with:
- multiple saved roots
- recursive-search semantics
- user-facing source labels

- [ ] **Step 4: Run tests to verify it passes**

Run narrow test target
Expected: PASS

## Chunk 5: Verification

### Task 5: End-to-end validation

**Files:**
- Verify only

- [ ] **Step 1: Run Linux verification**

Run: `ctest --test-dir build-linux-test --output-on-failure`
Expected: PASS

- [ ] **Step 2: Run Windows/WinUI build verification**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```
Expected: PASS

- [ ] **Step 3: Manual UX sanity check**

Verify:
- no recent dump -> direct empty state guidance
- registered search root with nested dump -> dump appears in recent list
- selecting a dump updates learned search location
