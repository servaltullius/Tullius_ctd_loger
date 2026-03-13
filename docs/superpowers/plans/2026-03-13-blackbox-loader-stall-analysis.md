# Blackbox Loader-Stall Analysis Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** module/thread lifecycle blackbox 신호를 추가하고, analyzer가 이를 `loader_stall_likely`와 related candidate reasoning에 반영하도록 만든다.

**Architecture:** plugin이 blackbox ring buffer에 `module_load/unload`, `thread_create/exit`를 기록하고, analyzer는 이를 `BlackboxFreezeSummary`로 집계한다. freeze consensus는 새 `Blackbox context` family를 사용해 loader-stall 설명력을 올리고, OutputWriter/evidence는 같은 aggregate를 공유한다. WinUI는 이번 단계 범위에서 제외한다.

**Tech Stack:** C++20, SKSE plugin blackbox shared memory, helper/minidump blackbox stream, analyzer/evidence pipeline, nlohmann/json, Linux/Windows build scripts

---

## Chunk 1: Contract And Event Vocabulary

### Task 1: Lock blackbox loader-stall contract with failing tests

**Files:**
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Create: `tests/blackbox_loader_stall_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- new event ids exist for `kModuleLoad`, `kModuleUnload`, `kThreadCreate`, `kThreadExit`
- analyzer source contract mentions `BlackboxFreezeSummary`
- output/report contract mentions blackbox-derived freeze reasons when present

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_blackbox_loader_stall_tests skydiag_analysis_engine_runtime_tests skydiag_output_snapshot_tests
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime|output_snapshot"
```

Expected: FAIL because the event ids / aggregate model do not exist yet

### Task 2: Add shared event vocabulary

**Files:**
- Modify: `shared/SkyrimDiagShared.h`
- Modify: `plugin/src/Blackbox.cpp`
- Modify: `dump_tool/src/AnalyzerInternals.cpp`
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`

- [ ] **Step 1: Add new event ids**

Extend `EventType` with:
- `kModuleLoad`
- `kModuleUnload`
- `kThreadCreate`
- `kThreadExit`

- [ ] **Step 2: Extend event name formatting**

Update analyzer event naming so these new event ids render stable text names.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R blackbox_loader_stall
```

Expected: PASS

## Chunk 2: Plugin Blackbox Emission

### Task 3: Lock plugin blackbox emission points with failing tests

**Files:**
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Assert source contracts for:
- module lifecycle hook/emission file exists or target file contains `kModuleLoad` / `kModuleUnload`
- thread lifecycle emission file exists or target file contains `kThreadCreate` / `kThreadExit`
- payload packing stores a stable hash and truncated display text

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R blackbox_loader_stall
```

Expected: FAIL because plugin is not emitting those events yet

### Task 4: Implement module/thread lifecycle blackbox emission

**Files:**
- Modify: `plugin/src/Blackbox.cpp`
- Modify: `plugin/src/SharedMemory.cpp`
- Modify: `plugin/src/EventSinks.cpp`
- Modify: `plugin/src/Heartbeat.cpp`
- Create or Modify: `plugin/src/ModuleLifecycle.cpp`
- Create or Modify: `plugin/src/ThreadLifecycle.cpp`
- Modify: `plugin/include/...` headers as needed
- Test: `tests/blackbox_loader_stall_tests.cpp`

- [ ] **Step 1: Add stable payload helpers**

Add helpers that pack:
- module hash
- truncated module basename / key segment
- thread id metadata

- [ ] **Step 2: Emit module lifecycle events**

Record `kModuleLoad` / `kModuleUnload` from the least invasive hook or lifecycle callback already available in the plugin.

- [ ] **Step 3: Emit thread lifecycle events**

Record `kThreadCreate` / `kThreadExit` from the plugin-side lifecycle point chosen for this repo.

- [ ] **Step 4: Keep ring-buffer discipline**

Ensure emission still respects:
- frozen-state suppression
- existing payload size limits
- no long path strings

- [ ] **Step 5: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R blackbox_loader_stall
```

Expected: PASS

## Chunk 3: Analyzer Aggregate

### Task 5: Lock analyzer blackbox aggregate with failing tests

**Files:**
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Require:
- `BlackboxFreezeSummary` model exists
- analyzer parses recent module/thread churn counts
- analyzer can expose recent non-system modules for freeze analysis

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime"
```

Expected: FAIL because no aggregate exists yet

### Task 6: Implement `BlackboxFreezeSummary`

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/AnalyzerInternals.h`
- Modify: `dump_tool/src/AnalyzerInternals.cpp`
- Create or Modify: `dump_tool/src/AnalyzerInternalsBlackbox.cpp`
- Create or Modify: `dump_tool/src/AnalyzerInternalsBlackbox.h`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Add the aggregate model**

Define fields for:
- loading-window bounded flag
- recent module load/unload counts
- recent thread create/exit counts
- module churn score
- thread churn score
- recent non-system modules

- [ ] **Step 2: Build the aggregate from `EventRow`**

Parse the recent bounded event window and compute stable counts/summaries.

- [ ] **Step 3: Attach aggregate to analysis result**

Store it on `AnalysisResult` or pass it directly into freeze consensus.

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime"
```

Expected: PASS

## Chunk 4: Freeze Consensus And Output

### Task 7: Lock loader-stall scoring updates with failing tests

**Files:**
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/candidate_consensus_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- `loading + blackbox churn` strengthens `loader_stall_likely`
- blackbox-only evidence does not overrule `deadlock_likely`
- related candidates can include recent non-system modules
- output/report contains blackbox-derived freeze reasons

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|output_snapshot"
```

Expected: FAIL because blackbox context is not part of freeze scoring yet

### Task 8: Implement blackbox-context freeze scoring

**Files:**
- Modify: `dump_tool/src/FreezeCandidateConsensus.h`
- Modify: `dump_tool/src/FreezeCandidateConsensus.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `tests/data/golden_summary_v2.json`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Add `Blackbox context` family input**

Feed the new aggregate into freeze consensus.

- [ ] **Step 2: Strengthen loader-stall scoring**

Use:
- loading context
- module churn
- thread churn
- recent non-system modules

to improve `loader_stall_likely` vs `freeze_ambiguous`.

- [ ] **Step 3: Keep deadlock precedence**

Ensure WCT cycle still wins over blackbox churn when both are present.

- [ ] **Step 4: Update evidence/recommendations/output**

Emit:
- blackbox-derived primary reasons
- recent non-system related candidates
- concise diagnostic summary for loader-stall context

- [ ] **Step 5: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|output_snapshot"
```

Expected: PASS

## Chunk 5: Full Verification And Branch Hygiene

### Task 9: Run full verification and commit

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
git add shared/SkyrimDiagShared.h plugin/src/Blackbox.cpp plugin/src/SharedMemory.cpp plugin/src/EventSinks.cpp plugin/src/Heartbeat.cpp dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/AnalyzerInternals.h dump_tool/src/AnalyzerInternals.cpp dump_tool/src/FreezeCandidateConsensus.h dump_tool/src/FreezeCandidateConsensus.cpp dump_tool/src/EvidenceBuilderEvidence.cpp dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/OutputWriter.cpp tests/CMakeLists.txt tests/blackbox_loader_stall_tests.cpp tests/freeze_candidate_consensus_tests.cpp tests/analysis_engine_runtime_tests.cpp tests/output_snapshot_tests.cpp tests/data/golden_summary_v2.json
git commit -m "feat: add blackbox loader stall analysis"
```
