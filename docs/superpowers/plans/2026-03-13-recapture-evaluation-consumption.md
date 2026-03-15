# Recapture Evaluation Consumption Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** incident manifest의 `recapture_evaluation`을 evidence, recommendation, summary/report 설명 계층에 연결한다.

**Architecture:** helper가 남긴 recapture metadata를 analyzer 점수에는 섞지 않고, output/evidence/recommendation에서만 소비한다. 이번 단계는 explanation layer만 강화하고 candidate scoring은 유지한다.

**Tech Stack:** C++20, dump tool output pipeline, evidence builders, source-guard/runtime snapshot tests

---

## Chunk 1: Contract Tests

### Task 1: 소비 계약을 failing test로 잠근다

**Files:**
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: Write failing tests**

Cover:
- summary JSON/report exposes `RecaptureReasons`
- evidence/recommendation builders reference `recapture_evaluation`
- missing recapture metadata still normalizes safely

- [ ] **Step 2: Run focused tests to confirm failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_output_snapshot_tests skydiag_analysis_engine_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R "output_snapshot|analysis_engine_runtime"
```

Expected: FAIL because recapture consumption is incomplete

## Chunk 2: OutputWriter Consumption

### Task 2: summary/report에 recapture 이유를 고정 출력한다

**Files:**
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `dump_tool/src/OutputWriterInternals.cpp` if manifest discovery normalization needs tweaks
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Normalize recapture reasons in summary JSON**

Expose:
- `incident.recapture_evaluation.triggered`
- `incident.recapture_evaluation.target_profile`
- `incident.recapture_evaluation.reasons`
- `incident.recapture_evaluation.escalation_level`

- [ ] **Step 2: Add report lines for recapture reasons**

Print:
- `RecaptureTriggered`
- `RecaptureTargetProfile`
- `RecaptureReasons`
- `RecaptureEscalationLevel`

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R output_snapshot
```

Expected: PASS

## Chunk 3: Evidence And Recommendation Consumption

### Task 3: evidence/recommendation에 recapture context를 연결한다

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/EvidenceBuilderSummary.cpp` only if a small helper/reference is needed
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Add recapture context evidence item**

Describe:
- target profile
- reasons
- escalation level

Keep it clearly labeled as capture context, not root-cause evidence.

- [ ] **Step 2: Add target-profile-aware recommendations**

Rules:
- `crash_richer`: explain why richer crash capture was selected before full memory
- `crash_full`: explain that weaker analysis repeated through richer stage
- `freeze_snapshot_richer`: explain snapshot/ambiguity quality motive

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "output_snapshot|analysis_engine_runtime"
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
git add dump_tool/src/OutputWriter.cpp dump_tool/src/OutputWriterInternals.cpp dump_tool/src/EvidenceBuilderEvidence.cpp dump_tool/src/EvidenceBuilderRecommendations.cpp dump_tool/src/EvidenceBuilderSummary.cpp tests/output_snapshot_tests.cpp tests/analysis_engine_runtime_tests.cpp docs/superpowers/specs/2026-03-13-recapture-evaluation-consumption-design.md docs/superpowers/plans/2026-03-13-recapture-evaluation-consumption.md
git commit -m "feat: explain recapture evaluation in analysis output"
```
