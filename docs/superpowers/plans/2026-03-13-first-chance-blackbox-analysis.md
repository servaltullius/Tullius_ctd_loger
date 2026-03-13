# First-Chance Blackbox Analysis Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** suspicious first-chance exception telemetry를 blackbox에 추가하고, analyzer가 이를 freeze/weak-CTD 해석 보강에 사용하게 만든다.

**Architecture:** plugin은 benign filter + dedupe/rate-limit를 거친 `kFirstChanceException` event를 기록하고, analyzer는 이를 `FirstChanceSummary`로 집계한다. freeze consensus와 output은 aggregate만 소비하며, dump trigger 정책은 그대로 유지한다.

**Tech Stack:** C++20, SKSE plugin blackbox shared memory, vectored exception handler path, analyzer/evidence pipeline, source-guard tests, Linux/Windows builds

---

## Chunk 1: Contract And Event Vocabulary

### Task 1: Lock first-chance contract with failing tests

**Files:**
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- `kFirstChanceException` event id exists
- analyzer model mentions `FirstChanceSummary`
- output/report contains `first_chance_context` aggregate fields

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_blackbox_loader_stall_tests skydiag_analysis_engine_runtime_tests skydiag_output_snapshot_tests
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime|output_snapshot"
```

Expected: FAIL because the event id / aggregate / output fields do not exist yet

### Task 2: Add shared event vocabulary

**Files:**
- Modify: `shared/SkyrimDiagShared.h`
- Modify: `plugin/src/Blackbox.cpp`
- Modify: `dump_tool/src/AnalyzerInternals.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`

- [ ] **Step 1: Add the new event id**

Extend `EventType` with:
- `kFirstChanceException`

- [ ] **Step 2: Add stable event naming**

Update analyzer event-name formatting so the new event renders as `FirstChanceException`.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R blackbox_loader_stall
```

Expected: PASS

## Chunk 2: Plugin Telemetry Emission

### Task 3: Lock plugin filtering/emission points with failing tests

**Files:**
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Modify: `tests/crash_hook_mode_guard_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/crash_hook_mode_guard_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Assert source contracts for:
- benign first-chance filter path exists
- rate limit / dedupe state exists
- telemetry path does not widen crash dump trigger policy

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|crash_hook_mode_guard"
```

Expected: FAIL because suspicious first-chance telemetry is not emitted yet

### Task 4: Emit suspicious first-chance blackbox events

**Files:**
- Modify: `plugin/src/CrashHandler.cpp`
- Modify: `plugin/include/SkyrimDiag/Blackbox.h`
- Modify: `plugin/src/Blackbox.cpp`
- Modify: `plugin/src/PluginMain.cpp` if shared config/rate-limit state needs wiring
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/crash_hook_mode_guard_tests.cpp`

- [ ] **Step 1: Add suspicious/benign filter helper**

Reuse existing benign exception knowledge and add a helper for:
- ignore benign codes
- allow suspicious first-chance candidates

- [ ] **Step 2: Add dedupe/rate limit**

Rate limit by short time window and duplicate `(code, address/module)` signature.

- [ ] **Step 3: Emit blackbox payload**

Pack:
- exception code
- exception address or stable address bucket
- module hash
- short module label

- [ ] **Step 4: Keep dump trigger unchanged**

Do not change `ShouldRecordCrashForMode` or fatal-only crash policy.

- [ ] **Step 5: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|crash_hook_mode_guard"
```

Expected: PASS

## Chunk 3: Analyzer Aggregate

### Task 5: Lock analyzer aggregate with failing tests

**Files:**
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Require:
- `FirstChanceSummary` model exists
- analyzer can count recent suspicious first-chance events
- analyzer can surface repeated signatures and related non-system modules

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime"
```

Expected: FAIL because no aggregate exists yet

### Task 6: Implement `FirstChanceSummary`

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/AnalyzerInternals.h`
- Modify: `dump_tool/src/AnalyzerInternals.cpp`
- Create or Modify: `dump_tool/src/AnalyzerInternalsFirstChance.cpp`
- Test: `tests/blackbox_loader_stall_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Add aggregate model**

Fields:
- `has_context`
- `recent_count`
- `unique_signature_count`
- `loading_window_count`
- `repeated_signature_count`
- `recent_non_system_modules`

- [ ] **Step 2: Parse event window**

Build the aggregate from recent `EventRow` values and current loading context.

- [ ] **Step 3: Attach to analysis result**

Store on `AnalysisResult` and pass into freeze/crash evidence/scoring.

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "blackbox_loader_stall|analysis_engine_runtime"
```

Expected: PASS

## Chunk 4: Scoring And Output

### Task 7: Lock scoring/output behavior with failing tests

**Files:**
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- loading-window repeated first-chance strengthens `loader_stall_likely`
- first-chance does not override deadlock precedence
- output exposes `first_chance_context`
- report/evidence mentions repeated suspicious first-chance context

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|output_snapshot"
```

Expected: FAIL because first-chance context is not part of scoring/output yet

### Task 8: Implement first-chance scoring and output

**Files:**
- Modify: `dump_tool/src/FreezeCandidateConsensus.h`
- Modify: `dump_tool/src/FreezeCandidateConsensus.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `tests/data/golden_summary_v2.json`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Add `first_chance_context` family input**

Feed the aggregate into freeze consensus and weak CTD evidence.

- [ ] **Step 2: Strengthen loader-stall scoring conservatively**

Use:
- loading-window suspicious first-chance count
- repeated signatures
- recent non-system modules

to improve `loader_stall_likely` vs `freeze_ambiguous`.

- [ ] **Step 3: Preserve precedence**

Ensure:
- deadlock still wins
- first-chance-only cannot force a culprit

- [ ] **Step 4: Update evidence/recommendations/output**

Emit:
- `first_chance_context`
- concise repeated-signature reason text
- recent non-system module hints

- [ ] **Step 5: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|output_snapshot"
```

Expected: PASS

## Chunk 5: Full Verification And Commit

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
git add shared/SkyrimDiagShared.h plugin/include/SkyrimDiag/Blackbox.h plugin/src/Blackbox.cpp plugin/src/CrashHandler.cpp plugin/src/PluginMain.cpp dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/AnalyzerInternals.h dump_tool/src/AnalyzerInternals.cpp dump_tool/src/AnalyzerInternalsFirstChance.cpp dump_tool/src/FreezeCandidateConsensus.h dump_tool/src/FreezeCandidateConsensus.cpp dump_tool/src/EvidenceBuilderEvidence.cpp dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/OutputWriter.cpp tests/blackbox_loader_stall_tests.cpp tests/analysis_engine_runtime_tests.cpp tests/freeze_candidate_consensus_tests.cpp tests/output_snapshot_tests.cpp tests/data/golden_summary_v2.json
git commit -m "feat: add first-chance blackbox analysis"
```
