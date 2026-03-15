# MainWindow Helper Evidence Cleanup Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `MainWindow.xaml.cs`, `helper/src/main.cpp`, `EvidenceBuilderEvidence.cpp`, 그리고 source-guard split-source 읽기 코드를 동작 변경 없이 구조 분리한다.

**Architecture:** 기존 public contract는 유지하고 orchestrator 파일만 얇게 남긴다. 새 helper 파일은 책임별 partial/translation unit로 추가하고, 기존 테스트와 빌드로 회귀를 고정한다.

**Tech Stack:** WinUI 3/C#, C++20, CMake, ctest

---

## Chunk 1: WinUI code-behind split

### Task 1: `MainWindow.xaml.cs`를 partial 파일로 분리

**Files:**
- Create: `dump_tool_winui/MainWindow.Localization.cs`
- Create: `dump_tool_winui/MainWindow.DumpDiscovery.cs`
- Create: `dump_tool_winui/MainWindow.Analysis.cs`
- Create: `dump_tool_winui/MainWindow.Triage.cs`
- Create: `dump_tool_winui/MainWindow.Layout.cs`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] `MainWindow.xaml.cs`에서 로컬라이즈/분석/triage/layout 메서드를 책임별로 이동한다.
- [ ] 생성자, 필드, 공통 `SetBusy`만 본 파일에 남긴다.
- [ ] `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml` 실행
- [ ] `bash scripts/build-winui-from-wsl.sh` 실행
- [ ] 커밋 `refactor: split main window code-behind helpers`

## Chunk 2: helper bootstrap split

### Task 2: `helper/src/main.cpp`의 startup/loop/process helper 분리

**Files:**
- Create: `helper/src/HelperMainInternal.h`
- Create: `helper/src/HelperMain.Loop.cpp`
- Create: `helper/src/HelperMain.Process.cpp`
- Create: `helper/src/HelperMain.Startup.cpp`
- Modify: `helper/src/main.cpp`
- Modify: `helper/CMakeLists.txt`
- Test: `tests/helper_crash_autopen_config_tests.cpp`

- [ ] `main.cpp`의 helper 함수들을 startup / loop / process-exit로 옮긴다.
- [ ] `wmain`은 bootstrap orchestration만 남긴다.
- [ ] `cmake --build build-linux-test` 실행
- [ ] `ctest --test-dir build-linux-test --output-on-failure -R "helper_crash_autopen_config|pending_crash_analysis_guard|incident_manifest_schema"` 실행
- [ ] `bash scripts/build-win-from-wsl.sh` 실행
- [ ] 커밋 `refactor: split helper main loop helpers`

## Chunk 3: evidence builder split

### Task 3: `EvidenceBuilderEvidence.cpp` 분리

**Files:**
- Create: `dump_tool/src/EvidenceBuilderEvidencePipeline.h`
- Create: `dump_tool/src/EvidenceBuilderEvidence.Context.cpp`
- Create: `dump_tool/src/EvidenceBuilderEvidence.Crash.cpp`
- Create: `dump_tool/src/EvidenceBuilderEvidence.Freeze.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/CMakeLists.txt`
- Test: `tests/output_snapshot_tests.cpp`
- Test: `tests/analysis_engine_runtime_tests.cpp`

- [ ] helper functions를 context / crash / freeze 영역으로 이동한다.
- [ ] `BuildEvidenceItems` orchestrator만 본 파일에 남긴다.
- [ ] `ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|output_snapshot|blackbox_loader_stall"` 실행
- [ ] `bash scripts/build-win-from-wsl.sh` 실행
- [ ] 커밋 `refactor: split evidence builder helpers`

## Chunk 4: source-guard utility dedupe

### Task 4: split-source read helper 공통화

**Files:**
- Modify: `tests/SourceGuardTestUtils.h`
- Modify: `tests/plugin_rules_tests.cpp`
- Modify: `tests/event_detail_guard_tests.cpp`
- Modify: `tests/symbol_privacy_controls_tests.cpp`
- Modify: `tests/incident_manifest_schema_tests.cpp`
- Modify: `tests/triage_fields_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/summary_schema_fields_tests.cpp`
- Modify: `tests/diagnostic_logging_guard_tests.cpp`
- Modify: `tests/graphics_injection_integration_tests.cpp`
- Modify: `tests/crash_history_tests.cpp`
- Modify: `tests/blackbox_loader_stall_tests.cpp`
- Modify: `tests/system_module_guard_tests.cpp`
- Modify: `tests/plugin_stream_tests.cpp`

- [ ] companion source를 읽는 공통 helper를 추가한다.
- [ ] 반복된 수동 concat 코드를 helper 호출로 바꾼다.
- [ ] `ctest --test-dir build-linux-test --output-on-failure` 실행
- [ ] `bash scripts/build-win-from-wsl.sh` 실행
- [ ] `bash scripts/build-winui-from-wsl.sh` 실행
- [ ] 커밋 `refactor: deduplicate source guard companion reads`
