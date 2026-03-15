# Analyzer/OutputWriter/HangCapture Cleanup Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Analyzer.cpp`, `OutputWriter.cpp`, `HangCapture.cpp`를 책임 기준으로 분리해 유지보수성을 높이되, 동작은 유지한다.

**Architecture:** `Analyzer`와 `OutputWriter`는 orchestration을 본 파일에 남기고 helper를 별도 compilation unit으로 이동한다. `HangCapture`는 guard 단계와 confirmed capture 실행 단계를 분리하되, side effect 순서와 로그 문구는 유지한다.

**Tech Stack:** C++17, CMake/Ninja, nlohmann_json, existing regression tests, Windows build scripts

---

## Chunk 1: Analyzer Split

### Task 1: Analyzer 경계 고정

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp`
- Create: `dump_tool/src/AnalyzerPipeline.h`
- Test: `tests/analysis_engine_runtime_tests.cpp`
- Test: `tests/candidate_consensus_tests.cpp`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: `Analyzer.cpp`의 helper를 입력 파싱/CrashLogger/히스토리로 분류한다**

대상 helper:
- `ParseExceptionInfo`
- `ResolveFaultModule`
- `ParseBlackboxStream`
- `IntegratePluginScan`
- `DetermineHangLike`
- `IntegrateCrashLoggerLog`
- `ComputeSuspects`
- history 관련 helper 전부

- [ ] **Step 2: 현재 관련 회귀 테스트를 먼저 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"
```

Expected:
- PASS

### Task 2: Analyzer 입력/통합 helper 분리

**Files:**
- Create: `dump_tool/src/Analyzer.CaptureInputs.cpp`
- Create: `dump_tool/src/AnalyzerPipeline.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/CMakeLists.txt`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: exception/fault module/blackbox/plugin scan/hang-like/CrashLogger/suspect helper를 `Analyzer.CaptureInputs.cpp`로 이동한다**

- [ ] **Step 2: `Analyzer.cpp`는 orchestration과 top-level helper만 유지하게 정리한다**

- [ ] **Step 3: targeted 테스트를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"
```

Expected:
- PASS

### Task 3: Analyzer history helper 분리

**Files:**
- Create: `dump_tool/src/Analyzer.History.cpp`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/CMakeLists.txt`
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] **Step 1: incident profile/history path/load/append helper를 `Analyzer.History.cpp`로 이동한다**

- [ ] **Step 2: `AnalyzeDump`의 단계 순서를 다시 확인한다**

- [ ] **Step 3: targeted 테스트를 다시 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"
```

Expected:
- PASS

- [ ] **Step 4: Commit**

```bash
git add dump_tool/src/Analyzer.cpp dump_tool/src/Analyzer.CaptureInputs.cpp dump_tool/src/Analyzer.History.cpp dump_tool/src/AnalyzerPipeline.h dump_tool/CMakeLists.txt
git commit -m "refactor: split analyzer orchestration helpers"
```

## Chunk 2: OutputWriter Split

### Task 4: OutputWriter 경계 고정

**Files:**
- Modify: `dump_tool/src/OutputWriter.cpp`
- Create: `dump_tool/src/OutputWriterPipeline.h`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: summary/report/write 경계를 고정한다**

- [ ] **Step 2: 현재 output 관련 테스트를 먼저 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "output_snapshot|analysis_engine_runtime"
```

Expected:
- PASS

### Task 5: Summary/Report helper 분리

**Files:**
- Create: `dump_tool/src/OutputWriter.Summary.cpp`
- Create: `dump_tool/src/OutputWriter.Report.cpp`
- Create: `dump_tool/src/OutputWriterPipeline.h`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `dump_tool/CMakeLists.txt`
- Test: `tests/output_snapshot_tests.cpp`

- [ ] **Step 1: `BuildSummaryJson`을 `OutputWriter.Summary.cpp`로 이동한다**

- [ ] **Step 2: `BuildReportText`를 `OutputWriter.Report.cpp`로 이동한다**

- [ ] **Step 3: `WriteOutputs`는 write orchestration만 남기게 정리한다**

- [ ] **Step 4: output 관련 테스트를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "output_snapshot|analysis_engine_runtime"
```

Expected:
- PASS

- [ ] **Step 5: Commit**

```bash
git add dump_tool/src/OutputWriter.cpp dump_tool/src/OutputWriter.Summary.cpp dump_tool/src/OutputWriter.Report.cpp dump_tool/src/OutputWriterPipeline.h dump_tool/CMakeLists.txt
git commit -m "refactor: split output writer sections"
```

## Chunk 3: HangCapture Split

### Task 6: HangCapture 경계 고정

**Files:**
- Modify: `helper/src/HangCapture.cpp`
- Create: `helper/src/HangCaptureInternal.h`
- Test: `tests/helper_crash_autopen_config_tests.cpp`
- Test: `tests/helper_preflight_guard_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`

- [ ] **Step 1: guard 단계와 confirmed capture 실행 단계를 나눈다**

- [ ] **Step 2: helper 회귀 테스트를 먼저 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "helper_crash_autopen_config|helper_preflight_guard|incident_manifest"
```

Expected:
- PASS

### Task 7: guard/execution 분리

**Files:**
- Create: `helper/src/HangCapture.Guards.cpp`
- Create: `helper/src/HangCapture.Execute.cpp`
- Create: `helper/src/HangCaptureInternal.h`
- Modify: `helper/src/HangCapture.cpp`
- Modify: `helper/CMakeLists.txt`
- Test: `tests/helper_crash_autopen_config_tests.cpp`
- Test: `tests/helper_preflight_guard_tests.cpp`
- Test: `tests/incident_manifest_schema_tests.cpp`

- [ ] **Step 1: suppression/reset/WCT cycle helper를 `HangCapture.Guards.cpp`로 이동한다**

- [ ] **Step 2: confirmed hang 이후 dump/manifest/ETW 실행 경로를 `HangCapture.Execute.cpp`로 이동한다**

- [ ] **Step 3: `HandleHangTick`는 orchestration만 남기게 정리한다**

- [ ] **Step 4: helper 회귀 테스트를 다시 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "helper_crash_autopen_config|helper_preflight_guard|incident_manifest"
```

Expected:
- PASS

- [ ] **Step 5: Commit**

```bash
git add helper/src/HangCapture.cpp helper/src/HangCapture.Guards.cpp helper/src/HangCapture.Execute.cpp helper/src/HangCaptureInternal.h helper/CMakeLists.txt
git commit -m "refactor: split hang capture flow"
```

## Chunk 4: Final Verification

### Task 8: 전체 회귀 확인

**Files:**
- No code changes expected

- [ ] **Step 1: Linux 전체 테스트를 실행한다**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure
```

Expected:
- PASS

- [ ] **Step 2: Windows 빌드를 실행한다**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

Expected:
- SUCCESS

- [ ] **Step 3: 마지막 정리 커밋이 필요 없으면 브랜치를 마무리한다**
