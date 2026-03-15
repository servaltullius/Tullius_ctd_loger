# Triage UX Hierarchy Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Triage 화면의 정보 계층을 재배치해 CrashLogger 기준 reading path를 먼저 고정하고, 권장 조치와 충돌 후보를 더 빠르게 해석할 수 있게 만든다.

**Architecture:** WinUI surface는 `MainWindow.xaml`에서 section order와 visual emphasis를 조정하고, `MainWindowViewModel.cs`에서 grouped recommendation / conflict comparison / crash-context 라벨을 만든다. 기존 summary JSON과 recommendation 텍스트는 최대한 재사용하며, source-guard 테스트로 의도한 UX 구조를 고정한다.

**Tech Stack:** WinUI 3 XAML, C#, existing source-guard C++ tests

---

## Chunk 1: UX hierarchy and grouped actions

### Task 1: Add failing source-guard tests for the new reading path

**Files:**
- Modify: `tests/winui_xaml_tests.cpp`
- Modify: `tests/hook_framework_guard_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add assertions for:
- `CrashLogger context` / equivalent top context card wording
- grouped recommendations section names
- conflicting candidate comparison surface
- reviewer section being wrapped in an `Expander`
- `Fault module` appearing in crash-context area instead of top quick cards

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test --target skydiag_winui_xaml_tests skydiag_hook_framework_guard_tests && ctest --test-dir build-linux-test --output-on-failure -R "winui_xaml|hook_framework_guard"`
Expected: FAIL with missing strings / outdated layout guards

- [ ] **Step 3: Write minimal implementation**

Implement the smallest set of XAML and view-model changes to satisfy the new hierarchy without changing analysis behavior.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build-linux-test --output-on-failure -R "winui_xaml|hook_framework_guard"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/winui_xaml_tests.cpp tests/hook_framework_guard_tests.cpp \
  dump_tool_winui/MainWindow.xaml dump_tool_winui/MainWindow.xaml.cs \
  dump_tool_winui/MainWindowViewModel.cs docs/superpowers/specs/2026-03-13-triage-ux-hierarchy-design.md \
  docs/superpowers/plans/2026-03-13-triage-ux-hierarchy.md
git commit -m "feat: tighten triage reading order"
```

### Task 2: Reorder triage sections and group recommendations

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`

- [ ] **Step 1: Keep the top summary, then add CrashLogger-first context**

Surface a dedicated context card above the actionable candidate list that shows:
- referenced mod / processing-at-crash label
- evidence agreement
- next action

- [ ] **Step 2: Move fault-module emphasis downward**

Keep `Module+Offset` available, but present it in `Crash context` instead of the first-read area.

- [ ] **Step 3: Group recommendations**

Split recommendation display into grouped blocks derived from existing prefixes / candidate status:
- immediate action
- extra verification
- recapture or compare

- [ ] **Step 4: Improve conflicting-candidate display**

Show first and second conflicting candidates with their own family summaries in a comparison-friendly block.

- [ ] **Step 5: Demote reviewer controls**

Wrap review editing UI in a collapsed `Expander` or otherwise visually secondary section.

## Chunk 2: Verification

### Task 3: Run project verification for the UX change

**Files:**
- No code changes expected

- [ ] **Step 1: Run Linux verification**

Run:
```bash
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

- [ ] **Step 2: Run Windows builds**

Run:
```bash
scripts\\build-win.cmd
scripts\\build-winui.cmd
```

- [ ] **Step 3: Sanity-check executable startup**

Run:
```bash
build-win\\bin\\SkyrimDiagDumpToolCli.exe --help
build-winui\\SkyrimDiagDumpToolWinUI.exe
```

- [ ] **Step 4: Commit if verification uncovered required follow-up fixes**

```bash
git add -A
git commit -m "test: verify triage UX hierarchy changes"
```
