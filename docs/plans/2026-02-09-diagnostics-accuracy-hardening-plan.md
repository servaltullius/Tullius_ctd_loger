# Diagnostics Accuracy Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Improve practical root-cause accuracy by adding triage feedback fields, unknown-bucket crash recapture, symbolization health metrics, and richer ETW hang profile handling.

**Architecture:** Keep existing `plugin -> helper -> dump_tool` pipeline. Extend `dump_tool` summary schema for triage/metrics, extend `helper` with optional recapture policy and ETW profile fallback, and add a script that aggregates reviewed summaries by bucket for quality tracking.

**Tech Stack:** C++20, nlohmann/json, Win32/DbgHelp/WPR, Python 3 stdlib, CTest.

### Task 1: Add failing tests for schema/config/script surfaces

**Files:**
- Modify: `tests/helper_crash_autopen_config_tests.cpp`
- Create: `tests/summary_schema_fields_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing assertions**
- Add assertions for new helper config keys and ETW split profile keys in `dist/SkyrimDiagHelper.ini` and `helper/src/Config.cpp`.
- Add assertions for new summary fields (`triage`, `symbolization`) in `dump_tool/src/OutputWriter.cpp`.

**Step 2: Verify RED**
- Run: `cmake --build build-linux-test --target skydiag_helper_crash_autopen_config_tests skydiag_summary_schema_fields_tests`
- Run: `ctest --test-dir build-linux-test --output-on-failure`

### Task 2: Implement triage feedback loop surface (item 1)

**Files:**
- Modify: `dump_tool/src/OutputWriter.cpp`
- Create: `scripts/analyze_bucket_quality.py`
- Modify: `README.md`

**Step 1: Add triage metadata fields**
- Add `summary["triage"]` object with default review fields for post-analysis labeling.

**Step 2: Add aggregation script**
- Parse `*_SkyrimDiagSummary.json` files, group by `crash_bucket_key`, and output reviewed/unknown-rate summaries.

**Step 3: Verify GREEN**
- Run: `python3 scripts/analyze_bucket_quality.py --help`

### Task 3: Implement unknown-bucket recapture policy (item 2)

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `helper/src/main.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`

**Step 1: Add config keys**
- Add optional recapture controls (enable, threshold, timeout).

**Step 2: Add stats + decision flow**
- Track per-bucket unknown-module counts in helper output dir.
- Run headless analysis synchronously for crash dumps when policy is enabled.
- If unknown bucket count meets threshold and process is still alive, capture one extra FullMemory crash dump.

**Step 3: Verify GREEN**
- Run: `ctest --test-dir build-linux-test --output-on-failure`

### Task 4: Implement symbolization metrics/path telemetry (item 3)

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`

**Step 1: Add metrics in `AnalysisResult`**
- Include total frames, symbolized frames, source-line frames, and resolved symbol path/cache path.

**Step 2: Populate metrics during stackwalk formatting**
- Record per-frame symbolization success and source-line success.

**Step 3: Emit metrics in summary JSON**
- Add `summary["symbolization"]` object.

### Task 5: Implement ETW profile split/fallback (item 4)

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `helper/src/main.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`
- Modify: `README.md`

**Step 1: Split profile config**
- Add `EtwHangProfile` and optional `EtwHangFallbackProfile` (with backward compatibility for `EtwProfile`).

**Step 2: Fallback behavior**
- If primary profile start fails, retry with fallback profile; keep dump/WCT flow best-effort.

**Step 3: Verify GREEN**
- Run: `ctest --test-dir build-linux-test --output-on-failure`

### Task 6: Final verification

**Files:**
- (No new files expected)

**Step 1: Build + tests**
- Run: `cmake --build build-linux-test`
- Run: `ctest --test-dir build-linux-test --output-on-failure`

**Step 2: Change check**
- Run: `git status --short`
- Run: `git diff -- tests/CMakeLists.txt tests/helper_crash_autopen_config_tests.cpp tests/summary_schema_fields_tests.cpp dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/OutputWriter.cpp helper/include/SkyrimDiagHelper/Config.h helper/src/Config.cpp helper/src/main.cpp dist/SkyrimDiagHelper.ini scripts/analyze_bucket_quality.py README.md docs/plans/2026-02-09-diagnostics-accuracy-hardening-plan.md`

