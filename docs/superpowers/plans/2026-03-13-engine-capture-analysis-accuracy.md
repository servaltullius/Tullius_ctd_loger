# Engine Capture And Analysis Accuracy Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** helper의 dump 캡처 품질과 dump_tool의 원인 판정 정확도를 단계적으로 높여, CTD와 프리징에서 더 유의미한 원인 후보를 제시한다.

**Architecture:** phase 1은 `incident-specific dump profile + callback shaping + analyzer consumption`을 기존 helper/dump_tool 위에 얹는다. phase 2는 freeze 전용 품질 향상을 위해 PSS snapshot과 추가 blackbox/WCT 신호를 separate spike/implementation으로 분리한다.

**Tech Stack:** C++17, Win32/DbgHelp, existing helper + dump_tool architecture, nlohmann/json, source-guard tests, Linux/Windows build scripts

---

## Chunk 1: Dump Profile Abstraction

### Task 1: Lock the profile contract with failing tests

**Files:**
- Create: `tests/dump_profile_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/dump_profile_tests.cpp`

- [ ] **Step 1: Write the failing test**

Cover:
- crash / hang / manual / recapture capture kinds exist
- `DumpMode` no longer maps directly to one `MINIDUMP_TYPE`
- a richer crash profile can differ from hang/manual defaults

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build-linux-test --target skydiag_dump_profile_tests
ctest --test-dir build-linux-test --output-on-failure -R dump_profile
```

Expected: FAIL because dump profiles do not exist yet

### Task 2: Introduce profile selection units

**Files:**
- Create: `helper/include/SkyrimDiagHelper/DumpProfile.h`
- Create: `helper/src/DumpProfile.cpp`
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`
- Test: `tests/dump_profile_tests.cpp`
- Test: `tests/helper_crash_autopen_config_tests.cpp`

- [ ] **Step 1: Add profile model**

Define:
- `enum class CaptureKind { Crash, Hang, Manual, CrashRecapture }`
- `struct DumpProfile`
- helpers that derive effective profile from `HelperConfig + DumpMode + CaptureKind`

- [ ] **Step 2: Add config surface**

Keep `DumpMode=0/1/2` for compatibility, but add optional per-kind overrides only if the codebase stays readable. Do not expose a large matrix in the first pass.

- [ ] **Step 3: Run tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "dump_profile|helper_crash_autopen_config"
```

Expected: PASS

## Chunk 2: Callback-Shaped Dump Writing

### Task 3: Lock callback-based dump shaping with failing tests

**Files:**
- Create: `tests/dump_writer_guard_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/dump_writer_guard_tests.cpp`

- [ ] **Step 1: Write the failing test**

Assert that:
- `DumpWriter.cpp` defines a callback routine
- `MiniDumpWriteDump` is called with callback info, not `nullptr`
- effective flags are derived from a profile object, not raw `DumpMode`

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build-linux-test --target skydiag_dump_writer_guard_tests
ctest --test-dir build-linux-test --output-on-failure -R dump_writer_guard
```

Expected: FAIL because writer still uses a simple direct call

### Task 4: Refactor dump writer around profiles and callback shaping

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/DumpWriter.h`
- Modify: `helper/src/DumpWriter.cpp`
- Modify: `helper/src/CrashCapture.cpp`
- Modify: `helper/src/HangCapture.cpp`
- Modify: `helper/src/ManualCapture.cpp`
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Test: `tests/dump_writer_guard_tests.cpp`
- Test: `tests/plugin_stream_tests.cpp`
- Test: `tests/crash_capture_refactored_guard_tests.cpp`

- [ ] **Step 1: Change writer interface**

Replace raw `DumpMode` parameter with an effective `DumpProfile` or equivalent options struct.

- [ ] **Step 2: Add callback shaping**

Wire a callback that can:
- keep crash thread
- prefer WCT cycle/main thread context for hang/manual
- keep unloaded modules and thread/process data
- avoid defaulting to full memory

- [ ] **Step 3: Pass capture kind from callers**

Crash/hang/manual/recapture paths must explicitly request their capture kind instead of all sharing one flat mode.

- [ ] **Step 4: Run focused tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "dump_writer_guard|plugin_stream|crash_capture_refactored_guard"
```

Expected: PASS

## Chunk 3: Manifest And Recapture Policy

### Task 5: Extend manifest/recapture contract with failing tests

**Files:**
- Modify: `tests/incident_manifest_schema_tests.cpp`
- Modify: `tests/crash_recapture_policy_tests.cpp`
- Modify: `tests/pending_crash_analysis_guard_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`
- Test: `tests/pending_crash_analysis_guard_tests.cpp`

- [ ] **Step 1: Write the failing test**

Assert that:
- incident manifest includes effective dump profile or flags
- recapture policy can key off more than unknown fault module
- pending crash analysis still performs safe cleanup while using the richer policy

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "incident_manifest_schema|crash_recapture_policy|pending_crash_analysis_guard"
```

Expected: FAIL because current policy is too narrow

### Task 6: Implement profile-aware recapture and manifest reporting

**Files:**
- Modify: `helper/src/IncidentManifest.cpp`
- Modify: `helper/src/PendingCrashAnalysis.cpp`
- Modify: `helper/include/SkyrimDiagHelper/CrashRecapturePolicy.h`
- Modify: `helper/src/CrashCapture.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`
- Test: `tests/crash_recapture_policy_tests.cpp`
- Test: `tests/pending_crash_analysis_guard_tests.cpp`

- [ ] **Step 1: Record effective capture profile**

Persist profile kind / effective flags / recapture reason in the incident manifest.

- [ ] **Step 2: Expand recapture inputs**

Teach recapture policy to use:
- unknown fault module streak
- candidate conflict
- isolated object-ref streak
- repeated stackwalk fallback / analysis degradation

- [ ] **Step 3: Run tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "incident_manifest_schema|crash_recapture_policy|pending_crash_analysis_guard"
```

Expected: PASS

## Chunk 4: Symbol And Runtime Health Preflight

### Task 7: Lock new preflight checks with failing tests

**Files:**
- Modify: `tests/helper_preflight_guard_tests.cpp`
- Modify: `tests/symbol_privacy_controls_tests.cpp`
- Test: `tests/helper_preflight_guard_tests.cpp`
- Test: `tests/symbol_privacy_controls_tests.cpp`

- [ ] **Step 1: Write the failing test**

Assert the preflight mentions:
- `dbghelp.dll`
- `msdia140.dll`
- symbol cache/path/runtime health

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "helper_preflight_guard|symbol_privacy_controls"
```

Expected: FAIL because preflight does not yet check runtime symbol health

### Task 8: Implement symbol/runtime preflight

**Files:**
- Modify: `helper/src/CompatibilityPreflight.cpp`
- Modify: `dump_tool/src/AnalyzerInternalsStackwalkSymbols.cpp`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`
- Test: `tests/helper_preflight_guard_tests.cpp`
- Test: `tests/symbol_privacy_controls_tests.cpp`

- [ ] **Step 1: Add helper-side checks**

Report:
- effective dbghelp path/version
- DIA availability
- symbol cache/path health

- [ ] **Step 2: Add analyzer-side diagnostics plumbing**

Expose degraded symbol environment in analyzer diagnostics so the UI/report can distinguish engine weakness from environment weakness.

- [ ] **Step 3: Run tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "helper_preflight_guard|symbol_privacy_controls"
```

Expected: PASS

## Chunk 5: Analyzer Consumption Of New Signals

### Task 9: Lock analyzer scoring/evidence behavior with failing tests

**Files:**
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/candidate_consensus_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Write the failing test**

Cover:
- degraded stackwalk/symbol state changes diagnostics/evidence
- richer capture profile metadata flows into output/report
- recapture-related analysis weakness can affect candidate reasoning or recommendations

- [ ] **Step 2: Run tests to verify failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"
```

Expected: FAIL because analyzer does not yet consume the new capture quality signals

### Task 10: Feed new capture-quality signals into analyzer output

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/CandidateConsensus.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: Extend analysis model**

Add fields that describe effective capture profile, stackwalk quality, symbol/runtime health, and recapture hints.

- [ ] **Step 2: Update evidence/recommendations**

Make reports explain:
- whether the dump was profile-shaped or recaptured
- whether symbol/runtime degradation limited confidence
- whether another capture is needed and why

- [ ] **Step 3: Run tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"
```

Expected: PASS

## Chunk 6: Full Verification For Phase 1

### Task 11: Run the full verification matrix

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

- [ ] **Step 3: Run WinUI smoke**

Launch:
```bash
build-winui/SkyrimDiagDumpToolWinUI.exe
```

Expected: process starts and remains alive normally

## Chunk 7: Phase 2 Freeze Spike (Separate PR Or Feature Flag)

### Task 12: Spike PSS snapshot feasibility without destabilizing phase 1

**Files:**
- Create: `docs/spikes/pss-snapshot-evaluation.md`
- Modify: `helper/src/HangCapture.cpp`
- Modify: `helper/src/ManualCapture.cpp`
- Test: `tests/helper_preflight_guard_tests.cpp`

- [ ] **Step 1: Add a spike-only feature flag**

Keep PSS off by default. No silent behavior swap in phase 1.

- [ ] **Step 2: Prototype hang/manual snapshot export path**

Measure:
- dump success rate
- WCT consistency
- user-perceived overhead

- [ ] **Step 3: Document adoption decision**

Record whether PSS should be promoted to the default freeze path, remain opt-in, or be dropped.
