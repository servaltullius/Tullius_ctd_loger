# Recapture Policy Loop Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 반복되는 crash/freeze 분석 약점을 helper recapture policy에 연결해, 더 적절한 capture profile로 자동 재수집하게 만든다.

**Architecture:** 공통 recapture evaluation model을 도입하고, crash/freeze는 같은 trigger 평가를 공유하되 target profile 선택은 kind별로 분리한다. 이번 단계는 helper policy + pending analysis integration + incident manifest reason 기록까지만 다루고, UI 소비는 제외한다.

**Tech Stack:** C++17, helper pending analysis pipeline, incident manifest JSON, source-guard tests, Linux/Windows build scripts

---

## Chunk 1: Policy Contract

### Task 1: Lock recapture policy contract with failing tests

**Files:**
- Modify: `tests/crash_recapture_policy_tests.cpp`
- Modify: `tests/pending_crash_analysis_guard_tests.cpp`
- Modify: `tests/incident_manifest_schema_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`
- Test: `tests/pending_crash_analysis_guard_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`

- [ ] **Step 1: Write the failing tests**

Cover:
- policy distinguishes crash vs freeze target profiles
- crash trigger includes `symbol_runtime_degraded` and `first_chance_candidate_weak`
- freeze trigger includes `freeze_ambiguous` and `freeze_snapshot_fallback`
- manifest source contract includes recapture reasons and target profile

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_crash_recapture_policy_tests skydiag_pending_crash_analysis_guard_tests skydiag_incident_manifest_schema_tests
ctest --test-dir build-linux-test --output-on-failure -R "crash_recapture_policy|pending_crash_analysis_guard|incident_manifest_schema"
```

Expected: FAIL because policy/manifest do not yet expose the new model

## Chunk 2: Shared Recapture Evaluation Model

### Task 2: Introduce shared decision/result structures

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/CrashRecapturePolicy.h`
- Test: `tests/crash_recapture_policy_tests.cpp`

- [ ] **Step 1: Add recapture kind / reason / target profile model**

Define:
- `RecaptureKind`
- `RecaptureReason`
- `RecaptureTargetProfile`
- richer `CrashRecaptureDecision` / shared evaluation result

- [ ] **Step 2: Add crash and freeze evaluation helpers**

Keep crash and freeze separate at the API boundary, but share common repeated-weakness rules where possible.

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R crash_recapture_policy
```

Expected: PASS

## Chunk 3: Pending Analysis Integration

### Task 3: Extend pending crash analysis with new crash triggers

**Files:**
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Modify: `helper/src/CrashCapture.cpp` if richer target profile selection needs plumbing
- Test: `tests/pending_crash_analysis_guard_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`

- [ ] **Step 1: Feed new summary weakness signals into recapture decision**

Crash-side inputs now include:
- `symbolRuntimeDegraded`
- `firstChanceCandidateWeak`
- existing `candidateConflict`
- existing `referenceClueOnly`
- existing `stackwalkDegraded`

- [ ] **Step 2: Pick target profile conservatively**

Rules:
- first escalation: `crash_richer`
- only escalate to `crash_full` if policy says stronger escalation is needed

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "pending_crash_analysis_guard|crash_recapture_policy"
```

Expected: PASS

## Chunk 4: Freeze Recapture Hook And Manifest

### Task 4: Record freeze-side recapture evaluation and target profile

**Files:**
- Modify: `helper/src/IncidentManifest.cpp`
- Modify: `helper/src/HangCapture.cpp`
- Modify: `helper/src/ManualCapture.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`
- Test: `tests/pending_crash_analysis_guard_tests.cpp`

- [ ] **Step 1: Add freeze recapture evaluation inputs**

Hook in:
- `freeze_ambiguous`
- `freeze_snapshot_fallback`
- weak candidate support under strong blackbox/first-chance context

- [ ] **Step 2: Persist recapture evaluation to manifest**

Write:
- `recapture_evaluation.kind`
- `recapture_evaluation.triggered`
- `recapture_evaluation.reasons`
- `recapture_evaluation.target_profile`
- `recapture_evaluation.escalation_level`

- [ ] **Step 3: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "incident_manifest_schema|pending_crash_analysis_guard"
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
git add helper/include/SkyrimDiagHelper/CrashRecapturePolicy.h helper/src/PendingCrashAnalysis.cpp helper/src/IncidentManifest.cpp helper/src/HangCapture.cpp helper/src/ManualCapture.cpp tests/crash_recapture_policy_tests.cpp tests/pending_crash_analysis_guard_tests.cpp tests/incident_manifest_schema_tests.cpp docs/superpowers/specs/2026-03-13-recapture-policy-loop-design.md docs/superpowers/plans/2026-03-13-recapture-policy-loop.md
git commit -m "feat: expand recapture policy loop"
```
