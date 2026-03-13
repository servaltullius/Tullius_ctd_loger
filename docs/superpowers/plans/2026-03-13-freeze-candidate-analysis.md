# Freeze Candidate Analysis Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** freeze/hang/manual snapshot 계열 dump에서 `deadlock`, `loader stall`, `freeze ambiguous`를 구조화해 분석 결과와 JSON/report에 반영한다.

**Architecture:** analyzer에 freeze 전용 consensus 레이어를 추가한다. WCT + loading + snapshot + existing candidate family를 합쳐 `freeze_analysis`를 계산하고, evidence/recommendation/output은 이 동일 모델을 사용한다. WinUI는 이번 단계 범위에서 제외한다.

**Tech Stack:** C++17, existing analyzer/evidence/output pipeline, nlohmann/json, source-guard tests, Linux/Windows build scripts

---

## Chunk 1: Freeze Contract And Data Model

### Task 1: Lock freeze analysis contract with failing tests

**Files:**
- Create: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- freeze analysis state ids exist: `deadlock_likely`, `loader_stall_likely`, `freeze_candidate`, `freeze_ambiguous`
- output snapshot schema requires `freeze_analysis`
- analyzer runtime contract mentions freeze analysis fields in source

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_freeze_candidate_consensus_tests skydiag_analysis_engine_runtime_tests skydiag_output_snapshot_tests
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|analysis_engine_runtime|output_snapshot"
```

Expected: FAIL because freeze analysis model does not exist yet

### Task 2: Add freeze analysis model types

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Create: `dump_tool/src/FreezeCandidateConsensus.h`
- Create: `dump_tool/src/FreezeCandidateConsensus.cpp`
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`

- [ ] **Step 1: Add model types**

Define:
- freeze state id enum/string contract
- support quality enum/string contract
- related candidate row struct
- freeze analysis result struct on `AnalysisResult`

- [ ] **Step 2: Add consensus entry point**

Add a unit that accepts:
- parsed WCT summary
- loading flags
- snapshot quality flags
- existing actionable candidates

and returns a freeze analysis result.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R freeze_candidate_consensus
```

Expected: PASS

## Chunk 2: WCT And Freeze Signal Parsing

### Task 3: Lock richer WCT parsing with failing tests

**Files:**
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Assert that parsed freeze signal summary can expose:
- cycle count
- candidate thread ids
- loading flag from capture decision
- snapshot transport / status if present in WCT JSON

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|analysis_engine_runtime"
```

Expected: FAIL because WCT model is too shallow

### Task 4: Extend WCT parsing helpers

**Files:**
- Modify: `dump_tool/src/WctTypes.h`
- Modify: `dump_tool/src/AnalyzerInternalsWct.cpp`
- Modify: `dump_tool/src/Analyzer.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Expand WCT model**

Add a richer parsed summary with:
- `cycle_threads`
- `longest_wait_tid`
- `loading_context`
- `pss_snapshot_requested`
- `pss_snapshot_used`
- `dump_transport`

- [ ] **Step 2: Feed summary into analyzer**

Teach analyzer to parse WCT once and store freeze-oriented summary fields in `AnalysisResult`.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|analysis_engine_runtime"
```

Expected: PASS

## Chunk 3: Freeze Consensus And Candidate Reasoning

### Task 5: Lock freeze state scoring with failing tests

**Files:**
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/candidate_consensus_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- WCT cycle -> `deadlock_likely`
- loading + freeze context -> `loader_stall_likely`
- weak/partial signals -> `freeze_candidate`
- conflicting/weak signals -> `freeze_ambiguous`
- existing crash candidate consensus remains unchanged for crash-like dumps

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|candidate_consensus"
```

Expected: FAIL because freeze scoring does not exist yet

### Task 6: Implement freeze consensus scoring

**Files:**
- Create: `dump_tool/src/FreezeCandidateConsensus.cpp`
- Create: `dump_tool/src/FreezeCandidateConsensus.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`

- [ ] **Step 1: Implement family scoring**

Implement four families:
- WCT
- Loading
- Snapshot quality
- Existing actionable candidate support

- [ ] **Step 2: Separate state from candidate**

Return both:
- freeze state classification
- related candidate list with confidence

Do not reuse crash `cross_validated` as the freeze state.

- [ ] **Step 3: Update evidence and recommendations**

Make evidence/report explain:
- why state was classified as deadlock/stall/ambiguous
- whether snapshot support improved capture quality

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|candidate_consensus"
```

Expected: PASS

## Chunk 4: Output Contract

### Task 7: Lock summary/report output with failing tests

**Files:**
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/data/golden_summary_v2.json`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Require:
- `freeze_analysis` block in summary JSON
- state/confidence/support_quality/primary_reasons/related_candidates fields
- report text mentions structured freeze analysis when present

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R output_snapshot
```

Expected: FAIL because output contract does not contain freeze analysis

### Task 8: Implement OutputWriter freeze block

**Files:**
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `tests/data/golden_summary_v2.json`
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Add summary JSON block**

Write:
- `freeze_analysis.state_id`
- `freeze_analysis.confidence`
- `freeze_analysis.support_quality`
- `freeze_analysis.primary_reasons`
- `freeze_analysis.related_candidates`

- [ ] **Step 2: Add report text section**

Emit a compact `FreezeAnalysis:` section in report text only when freeze analysis exists.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R output_snapshot
```

Expected: PASS

## Chunk 5: Full Verification

### Task 9: Run full verification matrix

**Files:**
- Verify only

- [ ] **Step 1: Run full Linux verification**

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

- [ ] **Step 3: Run smoke checks**

Run:
```bash
build-win/bin/SkyrimDiagDumpToolCli.exe --help
build-winui/SkyrimDiagDumpToolWinUI.exe
```

Expected:
- CLI prints usage
- WinUI starts normally

- [ ] **Step 4: Commit**

```bash
git add dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/WctTypes.h dump_tool/src/AnalyzerInternalsWct.cpp dump_tool/src/FreezeCandidateConsensus.h dump_tool/src/FreezeCandidateConsensus.cpp dump_tool/src/EvidenceBuilderEvidence.cpp dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/OutputWriter.cpp tests/freeze_candidate_consensus_tests.cpp tests/analysis_engine_runtime_tests.cpp tests/candidate_consensus_tests.cpp tests/output_snapshot_tests.cpp tests/data/golden_summary_v2.json
git commit -m "feat: add freeze candidate analysis"
```
