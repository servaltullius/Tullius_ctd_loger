# First-Chance CTD Candidate Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `first_chance_context`를 EXE/system-victim CTD의 actionable candidate 보강 family로 연결해, 약한 fatal victim CTD에서 candidate confidence와 triage explanation을 개선한다.

**Architecture:** 기존 actionable candidate scoring에 `first_chance_context` family를 추가한다. `FirstChanceSummary`는 이미 있으므로, 이번 단계는 EXE/system-victim gating, candidate linkage, evidence/recommendation/output 연결에 집중한다. first-chance 단독 승격은 금지하고, 기존 strong DLL culprit나 freeze rules는 건드리지 않는다.

**Tech Stack:** C++20, dump_tool candidate consensus/evidence pipeline, source-guard tests, Linux/Windows verification

---

## Chunk 1: Candidate Scoring Contract

### Task 1: Lock CTD first-chance candidate contract with failing tests

**Files:**
- Modify: `tests/candidate_consensus_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- candidate consensus knows about `first_chance_context`
- EXE/system-victim CTD can consume `FirstChanceSummary`
- first-chance only cannot promote a candidate

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_candidate_consensus_tests skydiag_analysis_engine_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|analysis_engine_runtime"
```

Expected: FAIL because CTD candidate consensus does not yet use first-chance context

### Task 2: Add `first_chance_context` family to EXE/system-victim scoring

**Files:**
- Modify: `dump_tool/src/CandidateConsensus.cpp`
- Modify: `dump_tool/src/CandidateConsensus.h` if helper structures need extension
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/Analyzer.h` if CTD candidate inputs need explicit first-chance linkage fields
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Gate by victim class**

Only evaluate `first_chance_context` for:
- game exe fault module
- system module fault module

- [ ] **Step 2: Require candidate linkage**

Linkage can come from:
- candidate module filename matching recent first-chance module
- candidate inferred mod/plugin mapped from that module
- existing object-ref/stack/resource candidate already present

- [ ] **Step 3: Add conservative score bonus**

Rules:
- first-chance only => no promotion
- object-ref + first-chance => modest bonus
- actionable stack + first-chance => modest bonus
- object-ref + stack + first-chance => stronger bonus

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|analysis_engine_runtime"
```

Expected: PASS

## Chunk 2: Evidence And Recommendation Contract

### Task 3: Lock explanation/output behavior with failing tests

**Files:**
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- candidate supporting families can include `first_chance_context`
- output/report mentions repeated suspicious first-chance for CTD candidate explanation
- output remains aggregate-only

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|output_snapshot"
```

Expected: FAIL because output/evidence does not yet mention CTD first-chance candidate support

### Task 4: Emit concise CTD first-chance explanation

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/EvidenceBuilderSummary.cpp` if summary wording changes are needed
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `tests/data/golden_summary_v2.json` if schema contract needs a stable family example
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`

- [ ] **Step 1: Add candidate-level explanation text**

Explain:
- repeated suspicious first-chance existed
- why it supports this candidate
- which recent non-system module(s) were involved

- [ ] **Step 2: Add recommendation text**

Guide:
- inspect repeated first-chance module path first
- do not immediately widen to generic EXE crash triage

- [ ] **Step 3: Keep output aggregate-only**

Do not expose raw event flood; only family ids and concise aggregate text.

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "candidate_consensus|output_snapshot"
```

Expected: PASS

## Chunk 3: Full Verification And Commit

### Task 5: Run full verification and commit

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
git add dump_tool/src/CandidateConsensus.cpp dump_tool/src/CandidateConsensus.h dump_tool/src/Analyzer.cpp dump_tool/src/Analyzer.h dump_tool/src/EvidenceBuilderEvidence.cpp dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/EvidenceBuilderSummary.cpp dump_tool/src/OutputWriter.cpp tests/candidate_consensus_tests.cpp tests/analysis_engine_runtime_tests.cpp tests/output_snapshot_tests.cpp tests/data/golden_summary_v2.json
git commit -m "feat: boost exe ctd candidates with first-chance context"
```
