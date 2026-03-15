# Actionable Candidate Naming Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** actionable candidate가 친화 라벨 대신 실제로 찾을 수 있는 파일 기반 식별자를 대표 이름으로 쓰게 만든다.

**Architecture:** backend candidate naming helper가 `plugin filename > dll filename > mod folder name > fallback` 규칙으로 `primary_identifier`를 계산하고, WinUI와 output 계층은 그 값을 대표 이름으로 공통 사용한다. 친화 라벨은 `secondary_label`로만 남긴다.

**Tech Stack:** C++20 dump tool core, WinUI C# view model, JSON summary output, source-guard tests

---

## Chunk 1: Contract Tests

### Task 1: naming policy를 failing test로 잠근다

**Files:**
- Modify: `tests/candidate_consensus_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/winui_xaml_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- representative name prefers plugin filename over friendly label
- representative name prefers dll filename over mod folder name when plugin is absent
- WinUI display uses representative identifier, not raw fallback label

- [ ] **Step 2: Run focused tests to confirm failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_candidate_consensus_tests skydiag_output_snapshot_tests skydiag_winui_xaml_tests
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|output_snapshot|winui_xaml"
```

Expected: FAIL because current naming still surfaces friendly labels too early

## Chunk 2: Backend Naming Policy

### Task 2: backend candidate naming helper를 도입한다

**Files:**
- Modify: `dump_tool/src/CandidateConsensus.cpp`
- Modify: `dump_tool/src/CandidateConsensus.h`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Add naming helper**

Rules:
- `plugin_name`
- `module_filename`
- `mod_name`
- fallback label

- [ ] **Step 2: Preserve secondary label**

Keep friendly label/mod folder as auxiliary text instead of the primary identifier.

- [ ] **Step 3: Update summary/output serialization**

Expose the representative identifier consistently in output JSON/report paths that already serialize candidate names.

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|output_snapshot"
```

Expected: PASS

## Chunk 3: WinUI Consumption

### Task 3: WinUI가 representative identifier를 대표 이름으로 쓰게 한다

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Parse any new naming fields if needed**

Only add fields if backend serialization requires them.

- [ ] **Step 2: Use representative identifier for visible titles**

Apply to:
- primary candidate text
- candidate cards
- conflict rows
- share/clipboard summary where applicable

- [ ] **Step 3: Keep secondary label as a subordinate description**

Do not show friendly label as the main title.

- [ ] **Step 4: Run focused test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R winui_xaml
```

Expected: PASS

## Chunk 4: Full Verification And Commit

### Task 4: Run verification matrix and commit

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
git add dump_tool/src/CandidateConsensus.cpp dump_tool/src/CandidateConsensus.h dump_tool/src/OutputWriter.cpp dump_tool_winui/AnalysisSummary.cs dump_tool_winui/MainWindowViewModel.cs tests/candidate_consensus_tests.cpp tests/output_snapshot_tests.cpp tests/winui_xaml_tests.cpp docs/superpowers/specs/2026-03-13-actionable-candidate-naming-design.md docs/superpowers/plans/2026-03-13-actionable-candidate-naming.md
git commit -m "feat: prefer file identifiers for actionable candidates"
```
