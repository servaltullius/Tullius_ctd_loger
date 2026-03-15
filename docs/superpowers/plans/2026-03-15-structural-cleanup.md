# Structural Cleanup Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `MainWindowViewModel`와 `PendingCrashAnalysis`를 책임 기준으로 분리해 유지보수성을 높이되, 동작은 유지한다.

**Architecture:** WinUI는 partial class 분리로 public surface를 보존하고, helper는 compilation unit 분리로 정책 계산과 side effect를 나눈다. 출력, 스키마, 바인딩 이름은 바꾸지 않는다.

**Tech Stack:** C++17 helper/dump_tool, C# WinUI 3, CMake/Ninja, existing guard tests

---

## Chunk 1: MainWindowViewModel Partial Split

### Task 1: 현재 ViewModel 경계 고정

**Files:**
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: 분리 대상 메서드 목록을 주석 없이 식별한다**

기준:
- dump discovery
- recommendation/recapture
- candidate display/conflict
- share text

- [ ] **Step 2: source-guard가 현재 파일명을 강제하지 않는지 확인한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected:
- PASS

- [ ] **Step 3: commit 준비 없이 다음 task로 진행한다**

### Task 2: Dump discovery / recommendation partial 파일 생성

**Files:**
- Create: `dump_tool_winui/MainWindowViewModel.DumpDiscovery.cs`
- Create: `dump_tool_winui/MainWindowViewModel.Recommendations.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: `PopulateDumpDiscovery` 관련 메서드를 `MainWindowViewModel.DumpDiscovery.cs`로 이동한다**

- [ ] **Step 2: recommendation grouping / recapture context 메서드를 `MainWindowViewModel.Recommendations.cs`로 이동한다**

- [ ] **Step 3: `MainWindowViewModel.cs`에서 중복 using / 빈 줄 / 정렬을 정리한다**

- [ ] **Step 4: WinUI source-guard를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected:
- PASS

### Task 3: candidate/share partial 파일 생성

**Files:**
- Create: `dump_tool_winui/MainWindowViewModel.Candidates.cs`
- Create: `dump_tool_winui/MainWindowViewModel.ShareText.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: candidate display / agreement / conflict helper를 `MainWindowViewModel.Candidates.cs`로 이동한다**

- [ ] **Step 2: clipboard/community share text helper를 `MainWindowViewModel.ShareText.cs`로 이동한다**

- [ ] **Step 3: WinUI source-guard를 다시 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected:
- PASS

- [ ] **Step 4: WinUI Windows build를 실행한다**

Run:
```bash
bash scripts/build-winui-from-wsl.sh
```

Expected:
- SUCCESS

- [ ] **Step 5: Commit**

```bash
git add dump_tool_winui/MainWindowViewModel*.cs tests/winui_xaml_tests.cpp
git commit -m "refactor: split main window view model by responsibility"
```

## Chunk 2: PendingCrashAnalysis Split

### Task 4: decision/execution 경계 고정

**Files:**
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Test: `tests/pending_crash_analysis_guard_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`

- [ ] **Step 1: summary load / recapture decision / target profile mapping / execution side effect 경계를 메모 수준으로 정리한다**

- [ ] **Step 2: 현재 helper 관련 회귀 테스트를 먼저 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "pending_crash_analysis|crash_recapture_policy|incident_manifest"
```

Expected:
- PASS

### Task 5: decision compilation unit 분리

**Files:**
- Create: `helper/src/PendingCrashAnalysis.Decision.cpp`
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Modify: `helper/CMakeLists.txt` or relevant helper source list if needed
- Test: `tests/pending_crash_analysis_guard_tests.cpp`

- [ ] **Step 1: summary 로드 / target mapping / suffix helper / decision 계산 관련 static helper를 `PendingCrashAnalysis.Decision.cpp`로 이동한다**

- [ ] **Step 2: 기존 entrypoint가 새 helper를 통해 같은 순서로 decision을 받게 정리한다**

- [ ] **Step 3: helper 관련 회귀 테스트를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "pending_crash_analysis|crash_recapture_policy|incident_manifest"
```

Expected:
- PASS

### Task 6: execution compilation unit 분리

**Files:**
- Create: `helper/src/PendingCrashAnalysis.Execute.cpp`
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Modify: helper source list if needed
- Test: `tests/pending_crash_analysis_guard_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`

- [ ] **Step 1: manifest update / recapture dump 실행 / recapture incident manifest write를 `PendingCrashAnalysis.Execute.cpp`로 이동한다**

- [ ] **Step 2: 로그 메시지와 behavior가 바뀌지 않았는지 확인한다**

- [ ] **Step 3: helper 관련 회귀 테스트를 다시 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "pending_crash_analysis|crash_recapture_policy|incident_manifest"
```

Expected:
- PASS

- [ ] **Step 4: Commit**

```bash
git add helper/src/PendingCrashAnalysis*.cpp helper/CMakeLists.txt tests/pending_crash_analysis_guard_tests.cpp tests/crash_recapture_policy_tests.cpp tests/incident_manifest_schema_tests.cpp
git commit -m "refactor: split pending crash analysis flow"
```

## Chunk 3: Final Verification

### Task 7: 전체 회귀 확인

**Files:**
- No code changes expected

- [ ] **Step 1: Linux 전체 테스트를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure
```

Expected:
- PASS

- [ ] **Step 2: 필요 시 Windows WinUI 빌드를 다시 실행한다**

Run:
```bash
bash scripts/build-winui-from-wsl.sh
```

Expected:
- SUCCESS

- [ ] **Step 3: 정리 커밋이 더 필요 없으면 브랜치를 마무리한다**
