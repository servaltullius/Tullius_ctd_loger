# Freeze PSS Quality Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** freeze/hang/manual 경로에서 richer PSS snapshot flags와 WCT consensus metadata를 도입하고, analyzer가 이를 실제 freeze confidence와 근거 문구에 반영하게 만든다.

**Architecture:** helper는 `PSS_CAPTURE_VA_SPACE`와 `PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION`를 freeze snapshot에 추가하고, WCT는 2회 캡처 후 consensus summary를 JSON에 기록한다. dump analyzer는 새 WCT/PSS metadata를 파싱해 `FreezeCandidateConsensus`의 `support_quality`, `confidence`, `primary_reasons`를 보강하고, output은 이를 사용자 문장으로 노출한다.

**Tech Stack:** C++17, Windows PSS/WCT APIs, nlohmann/json, existing helper/analyzer/output pipeline, source-guard tests, Linux/Windows build scripts

---

## File Map

- Modify: `helper/src/PssSnapshot.cpp`
  - freeze snapshot flags를 richer freeze-oriented PSS flags로 확장
- Modify: `helper/src/WctCapture.cpp`
  - WCT 2-pass capture와 consensus summary JSON 추가
- Modify: `dump_tool/src/WctTypes.h`
  - WCT freeze summary contract에 consensus fields 추가
- Modify: `dump_tool/src/AnalyzerInternalsWct.cpp`
  - consensus fields 파싱과 freeze summary 모델 확장
- Modify: `dump_tool/src/FreezeCandidateConsensus.cpp`
  - consensus-backed freeze confidence/support quality 로직 추가
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.Summary.cpp`
- Modify: `dump_tool/src/OutputWriter.Report.cpp`
  - freeze quality/wct consensus 출력 연결
- Modify: `tests/wct_parsing_tests.cpp`
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
  - RED/GREEN regression guard

## Chunk 1: Richer Freeze PSS Flags

### Task 1: Lock richer PSS freeze flags with failing tests

**Files:**
- Modify: `tests/dump_writer_guard_tests.cpp`
- Test: `tests/dump_writer_guard_tests.cpp`

- [ ] **Step 1: Write the failing test**

Assert that freeze PSS capture source contains:
- `PSS_CAPTURE_VA_SPACE`
- `PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION`

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build-linux-test --target skydiag_dump_writer_guard_tests
ctest --test-dir build-linux-test --output-on-failure -R dump_writer_guard
```

Expected: FAIL because current PSS freeze flags do not include richer VA-space fields.

### Task 2: Implement richer freeze PSS flags

**Files:**
- Modify: `helper/src/PssSnapshot.cpp`
- Modify: `tests/dump_writer_guard_tests.cpp`
- Test: `tests/dump_writer_guard_tests.cpp`

- [ ] **Step 1: Extend the freeze PSS flag set**

Add:
- `PSS_CAPTURE_VA_SPACE`
- `PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION`

Do not add:
- `PSS_CAPTURE_HANDLES`
- type-specific handle capture

- [ ] **Step 2: Run focused test to verify it passes**

Run:
```bash
cmake --build build-linux-test --target skydiag_dump_writer_guard_tests
ctest --test-dir build-linux-test --output-on-failure -R dump_writer_guard
```

Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add helper/src/PssSnapshot.cpp tests/dump_writer_guard_tests.cpp
git commit -m "feat: widen freeze PSS snapshot flags"
```

## Chunk 2: WCT Consensus Metadata

### Task 3: Lock WCT consensus fields with failing tests

**Files:**
- Modify: `tests/wct_parsing_tests.cpp`
- Test: `tests/wct_parsing_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Require parsing support for:
- `capture_passes`
- `cycle_consensus`
- `repeated_cycle_tids`
- `consistent_loading_signal`
- `longest_wait_tid_consensus`

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build-linux-test --target skydiag_wct_parsing_tests
ctest --test-dir build-linux-test --output-on-failure -R wct_parsing
```

Expected: FAIL because WCT parser does not yet model consensus fields.

### Task 4: Implement helper-side WCT 2-pass consensus

**Files:**
- Modify: `helper/src/WctCapture.cpp`
- Modify: `dump_tool/src/WctTypes.h`
- Modify: `dump_tool/src/AnalyzerInternalsWct.cpp`
- Modify: `tests/wct_parsing_tests.cpp`
- Test: `tests/wct_parsing_tests.cpp`

- [ ] **Step 1: Add a second WCT capture pass**

Capture WCT twice per process with a short bounded delay and compute:
- total pass count
- repeated cycle tids
- cycle consensus boolean
- loading-signal consistency boolean
- longest-wait tid consensus boolean

- [ ] **Step 2: Preserve existing thread arrays**

Do not replace the existing `threads[]` contract. Add consensus summary fields alongside it.

- [ ] **Step 3: Teach analyzer-side WCT parsing about the new fields**

Extend `WctFreezeSummary` and parsing helpers to store the consensus summary.

- [ ] **Step 4: Run focused tests to verify they pass**

Run:
```bash
cmake --build build-linux-test --target skydiag_wct_parsing_tests
ctest --test-dir build-linux-test --output-on-failure -R wct_parsing
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add helper/src/WctCapture.cpp dump_tool/src/WctTypes.h dump_tool/src/AnalyzerInternalsWct.cpp tests/wct_parsing_tests.cpp
git commit -m "feat: add freeze WCT consensus metadata"
```

## Chunk 3: Freeze Consensus Consumption

### Task 5: Lock consensus-backed freeze confidence with failing tests

**Files:**
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- repeated cycle consensus raises `deadlock_likely` confidence over single-pass live WCT
- consistent loading signal plus first-chance/blackbox remains `loader_stall_likely`
- richer snapshot-backed capture upgrades `support_quality`
- fallback/single-pass paths stay conservative

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_freeze_candidate_consensus_tests skydiag_analysis_engine_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|analysis_engine_runtime"
```

Expected: FAIL because freeze consensus does not yet consume WCT consensus or richer snapshot quality.

### Task 6: Implement analyzer freeze-quality consumption

**Files:**
- Modify: `dump_tool/src/FreezeCandidateConsensus.cpp`
- Modify: `tests/freeze_candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/freeze_candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Extend support quality**

Introduce richer support quality distinctions such as:
- `snapshot_consensus_backed`
- `snapshot_backed`
- `snapshot_fallback`
- `live_process`

- [ ] **Step 2: Use consensus fields conservatively**

Rules:
- repeated cycle consensus can raise deadlock confidence
- consistent loading signal can raise loader-stall confidence
- snapshot richness can strengthen support quality and reasons
- no state change from snapshot metadata alone

- [ ] **Step 3: Run focused tests to verify they pass**

Run:
```bash
cmake --build build-linux-test --target skydiag_freeze_candidate_consensus_tests skydiag_analysis_engine_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R "freeze_candidate_consensus|analysis_engine_runtime"
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add dump_tool/src/FreezeCandidateConsensus.cpp tests/freeze_candidate_consensus_tests.cpp tests/analysis_engine_runtime_tests.cpp
git commit -m "feat: use WCT consensus for freeze confidence"
```

## Chunk 4: Output Propagation

### Task 7: Lock richer freeze capture wording with failing tests

**Files:**
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Require summary/report text to distinguish:
- snapshot-backed consensus
- live-process single-pass capture
- fallback snapshot paths

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build-linux-test --target skydiag_output_snapshot_tests
ctest --test-dir build-linux-test --output-on-failure -R output_snapshot
```

Expected: FAIL because output does not yet mention richer freeze capture quality.

### Task 8: Implement freeze quality output propagation

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.Summary.cpp`
- Modify: `dump_tool/src/OutputWriter.Report.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Surface support quality and WCT consensus in output**

Make freeze summary/report mention:
- whether repeated cycle consensus existed
- whether capture was snapshot-backed or live-process
- whether snapshot fallback occurred

- [ ] **Step 2: Keep wording conservative**

Do not claim “confirmed deadlock” unless the state already resolves to `deadlock_likely`.

- [ ] **Step 3: Run focused test to verify it passes**

Run:
```bash
cmake --build build-linux-test --target skydiag_output_snapshot_tests
ctest --test-dir build-linux-test --output-on-failure -R output_snapshot
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/OutputWriter.Summary.cpp dump_tool/src/OutputWriter.Report.cpp tests/output_snapshot_tests.cpp
git commit -m "feat: expose freeze capture quality evidence"
```

## Full Verification

### Task 9: Run full verification and push

**Files:**
- Modify: none
- Test: full repo verification

- [ ] **Step 1: Run Linux verification**

```bash
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

Expected: `56/56` pass or current updated total with 0 failures

- [ ] **Step 2: Run Windows builds**

```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

Expected: both succeed

- [ ] **Step 3: Push**

```bash
git push origin main
```
