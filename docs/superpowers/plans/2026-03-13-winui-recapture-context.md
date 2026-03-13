# WinUI Recapture Context Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** WinUI가 `incident.recapture_evaluation`을 evidence/recommendation 설명 계층에서 읽고 보여주게 만든다.

**Architecture:** summary JSON에서 recapture metadata를 `AnalysisSummary`로 읽고, `MainWindowViewModel`이 표시용 텍스트를 계산하며, `MainWindow.xaml`/`.xaml.cs`는 recommendations 영역 안에서 작은 recapture context block을 조건부로 렌더링한다. 상단 KPI나 candidate scoring은 바꾸지 않는다.

**Tech Stack:** C#/.NET 8 WinUI, existing summary JSON parser, source-guard tests, Linux CMake test harness, Windows WinUI build

---

## Chunk 1: Contract Tests

### Task 1: WinUI source contracts를 failing test로 잠근다

**Files:**
- Modify: `tests/winui_xaml_tests.cpp`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add guards for:
- `AnalysisSummary` parsing `incident.recapture_evaluation`
- `MainWindowViewModel` exposing recapture-context state/text
- `MainWindow.xaml` containing a recapture context block near recommendations
- `MainWindow.xaml.cs` toggling visibility with parsed state

- [ ] **Step 2: Run focused test to confirm failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_winui_xaml_tests
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected: FAIL because WinUI does not yet expose recapture context

## Chunk 2: Summary Model Parsing

### Task 2: `AnalysisSummary`에 recapture metadata를 추가한다

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Add recapture summary fields**

Add fields:
- `HasRecaptureEvaluation`
- `RecaptureTriggered`
- `RecaptureKind`
- `RecaptureTargetProfile`
- `RecaptureEscalationLevel`
- `RecaptureReasons`

- [ ] **Step 2: Parse `incident.recapture_evaluation` safely**

Rules:
- missing object => default values
- missing reasons => empty list
- no throw on old summary schema

- [ ] **Step 3: Run focused test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected: still FAIL until view model/XAML are updated

## Chunk 3: ViewModel Consumption

### Task 3: recapture context 표시용 상태를 계산한다

**Files:**
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Add view-model fields**

Add:
- `ShowRecaptureContext`
- `RecaptureContextTitle`
- `RecaptureContextDetails`

- [ ] **Step 2: Populate fields from `AnalysisSummary`**

Rules:
- show only when `HasRecaptureEvaluation && RecaptureTriggered`
- title stays explanation-focused, e.g. `Recapture context`
- details summarize `target_profile`, `escalation_level`, `reasons`
- do not modify `QuickPrimaryValue` / `QuickConfidenceValue`

- [ ] **Step 3: Keep recommendation grouping unchanged**

Only ensure recapture context complements the existing `RecaptureRecommendations` section.

- [ ] **Step 4: Run focused test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected: still FAIL until XAML/rendering is wired

## Chunk 4: XAML And Rendering

### Task 4: recommendations 영역에 recapture context block을 추가한다

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Add a conditional recapture context block in XAML**

Place it inside the recommendations card, above grouped recommendation sections.

Include:
- title text
- details text
- collapsed by default

- [ ] **Step 2: Wire visibility/text in `RenderSummary`**

Set:
- title text
- details text
- visibility from `_vm.ShowRecaptureContext`

- [ ] **Step 3: Run focused test to verify GREEN**

Run:
```bash
cmake --build build-linux-test --target skydiag_winui_xaml_tests
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected: PASS

## Chunk 5: Full Verification And Commit

### Task 5: Run verification matrix and commit

**Files:**
- Verify only

- [ ] **Step 1: Run Linux verification**

Run:
```bash
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

Expected: PASS

- [ ] **Step 2: Run Windows builds**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

Expected: PASS

- [ ] **Step 3: Smoke-check CLI**

Run:
```bash
/mnt/c/Windows/System32/cmd.exe /c "pushd Z:\\home\\kdw73\\Tullius_ctd_loger\\.worktrees\\exe-objectref-candidate-ui && build-win\\bin\\SkyrimDiagDumpToolCli.exe --help && popd"
```

Expected: usage text prints successfully

- [ ] **Step 4: Commit**

```bash
git add dump_tool_winui/AnalysisSummary.cs dump_tool_winui/MainWindowViewModel.cs dump_tool_winui/MainWindow.xaml dump_tool_winui/MainWindow.xaml.cs tests/winui_xaml_tests.cpp docs/superpowers/specs/2026-03-13-winui-recapture-context-design.md docs/superpowers/plans/2026-03-13-winui-recapture-context.md
git commit -m "feat: show recapture context in winui"
```
