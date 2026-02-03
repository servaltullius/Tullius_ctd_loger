# CrashLogger v1.18.0 C++ Exception Parsing + Alt-Tab Hang Suppression Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Improve diagnostic accuracy by parsing CrashLoggerSSE v1.18.0 `C++ EXCEPTION:` details and reduce false hang dumps caused by Alt-Tab/background pause.

**Architecture:** Keep parsing logic cross-platform in `dump_tool/src/CrashLoggerParseCore.h` (pure text parsing). Keep hang suppression policy in `helper/include/SkyrimDiagHelper/HangSuppression.h` (pure policy), with Win32 window-responsiveness detection staying in `helper/src/main.cpp`.

**Tech Stack:** C++20, CMake, (Linux) unit tests via `ctest`, (Windows) MSVC build + Python packaging script.

---

### Task 1: Add failing tests (CrashLogger v1.18 + hang suppression)

**Files:**
- Modify: `tests/crashlogger_parser_tests.cpp`
- Modify: `tests/hang_suppression_tests.cpp`

**Step 1: Write the failing test (CrashLogger C++ EXCEPTION block)**
- Add a log sample containing:
  - `C++ EXCEPTION:`
  - `\tType: ...`
  - `\tInfo: ...`
  - `\tThrow Location: ...`
  - `\tModule: ...`
- Assert parsed fields match expected.

**Step 2: Write the failing test (Alt-Tab suppression stays active when responsive)**
- Extend `EvaluateHangSuppression` test to expect suppression *after* the foreground grace period when:
  - hang was previously suppressed due to not-foreground
  - heartbeat did not advance yet
  - window is still responsive
  - not in loading screen

**Step 3: Run tests to confirm they fail**
Run: `cmake --build build-linux -j`
Expected: compile/test failure due to missing parser + new suppression reason.

---

### Task 2: Implement CrashLogger C++ exception parser (core)

**Files:**
- Modify: `dump_tool/src/CrashLoggerParseCore.h`

**Step 1: Add data model + parser**
- Add `CrashLoggerCppExceptionDetails` with fields:
  - `type`, `info`, `throw_location`, `module`
- Implement `ParseCrashLoggerCppExceptionDetailsAscii(std::string_view logUtf8)`:
  - Find `C++ EXCEPTION:` header
  - Parse indented lines with keys above
  - Stop at blank line or non-indented line

**Step 2: Run the parser test**
Run: `cmake --build build-linux --target skydiag_crashlogger_parser_tests -j && ./build-linux/bin/skydiag_crashlogger_parser_tests`
Expected: PASS.

---

### Task 3: Improve hang suppression to avoid Alt-Tab false dumps

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/HangSuppression.h`
- Modify: `helper/src/main.cpp`

**Step 1: Extend suppression reason**
- Add `HangSuppressionReason::kForegroundResponsive`

**Step 2: Extend policy function signature**
- Extend `EvaluateHangSuppression(...)` to accept:
  - `isLoading`
  - `isWindowResponsive`

**Step 3: Policy behavior**
- When returning to foreground after a not-foreground suppression episode:
  - still suppress during `ForegroundGraceSec`
  - after grace, keep suppressing if `!isLoading && isWindowResponsive`
  - stop suppressing once heartbeat advances

**Step 4: Wire responsiveness in helper**
- In hang path, resolve cached main window for PID
- Evaluate responsiveness via `SendMessageTimeoutW(...WM_NULL...)`

**Step 5: Run the suppression tests**
Run: `cmake --build build-linux --target skydiag_hang_suppression_tests -j && ./build-linux/bin/skydiag_hang_suppression_tests`
Expected: PASS.

---

### Task 4: Surface CrashLogger C++ exception details in analysis output

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/EvidenceBuilder.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Extend `AnalysisResult`**
- Add fields for CrashLogger C++ exception type/info/throw location/module.

**Step 2: Parse in Analyzer**
- When a CrashLogger log is found, parse the `C++ EXCEPTION` block and fill fields.
- Canonicalize the `Module:` name when possible using loaded dump modules.

**Step 3: Add evidence item**
- Add `Crash Logger C++ 예외 정보` evidence row summarizing fields.

**Step 4: Add output fields**
- Include fields in JSON summary + text report.
- Include a one-line summary segment in GUI “copy summary”.

**Step 5: Run unit tests**
Run: `ctest --test-dir build-linux -V`
Expected: `100% tests passed`.

---

### Task 5: Version bump + packaging

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `vcpkg.json`
- Modify: `CHANGELOG.md`

**Step 1: Bump version**
- Set version to `0.2.2` and add changelog entry.

**Step 2: Build + zip packaging (Windows mirror)**
- Sync WSL repo changes to the Windows mirror
- Build: `scripts\\build-win.cmd`
- Package: `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip`

