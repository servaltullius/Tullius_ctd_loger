# DumpTool English Default + Korean Toggle (I18n) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the DumpTool + generated outputs (Report/Summary/Evidence) default to **English** for Nexus release, while providing an in-app toggle to **한국어** that persists via an `.ini`.

**Architecture:** Add a small i18n layer:
- Cross-platform: `dump_tool/src/I18nCore.h` for `Language`, parsing, confidence labels, and UI label strings (no Win32).
- Windows-only: `dump_tool/src/DumpToolConfig.{h,cpp}` reads/writes `SkyrimDiagDumpTool.ini` next to the exe.
- Runtime: `AnalyzeOptions.language` drives localized strings from Analyzer/Evidence/Report. GUI toggle triggers re-analysis and rewrites outputs in the chosen language.

**Tech Stack:** C++20, CMake, (Linux) unit tests (`ctest`), (Windows) MSVC build + `scripts/package.py`.

---

### Task 1: TDD — i18n core tests (RED)

**Files:**
- Create: `tests/i18n_core_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing tests**
- Default language is English.
- Parsing `--lang ko` / `--lang en` works.
- Confidence labels map correctly:
  - EN: High/Medium/Low
  - KO: 높음/중간/낮음

**Step 2: Run tests to confirm failure**
Run: `cmake --build build-linux -j && ctest --test-dir build-linux -V`
Expected: failure because `I18nCore.h` does not exist yet.

---

### Task 2: Implement i18n core (GREEN)

**Files:**
- Create: `dump_tool/src/I18nCore.h`

**Step 1: Implement**
- `enum class Language { English, Korean }`
- `ParseLanguageToken(...)` (accept `en/english`, `ko/korean`)
- `ConfidenceLevel` + `ConfidenceLabel(Language, ConfidenceLevel)`
- UI label helpers for:
  - tab names, column headers, button labels, common headings (Conclusion/Basic info/etc.)

**Step 2: Run i18n tests**
Run: `cmake --build build-linux --target skydiag_i18n_core_tests -j && ./build-linux/bin/skydiag_i18n_core_tests`
Expected: PASS.

---

### Task 3: DumpTool config (ini) for language

**Files:**
- Create: `dump_tool/src/DumpToolConfig.h`
- Create: `dump_tool/src/DumpToolConfig.cpp`
- Modify: `dump_tool/src/main.cpp`

**Step 1: Implement ini load/save**
- Path: `<DumpToolExeDir>\\SkyrimDiagDumpTool.ini`
- Section/key: `[SkyrimDiagDumpTool] Language=en|ko`
- Default: English

**Step 2: CLI override**
- Add `--lang en|ko` to `SkyrimDiagDumpTool.exe` to override ini.
- Headless mode uses the resolved language too.

---

### Task 4: Localize Analyzer/Evidence/Report outputs

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/EvidenceBuilder.h`
- Modify: `dump_tool/src/EvidenceBuilder.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`

**Step 1: Plumb language**
- Add `AnalyzeOptions.language`
- Pass language into `BuildEvidenceAndSummary(...)`
- Ensure suspects/evidence/recommendations/summary are localized by language.

**Step 2: Confidence decoupling**
- Add `confidence_level` to evidence/suspects so UI coloring does not depend on localized strings.

---

### Task 5: GUI language toggle + label updates

**Files:**
- Modify: `dump_tool/src/GuiApp.h`
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Add toggle button**
- Add bottom button: `EN/KO` (or `Language: EN`)
- On click:
  - Flip language
  - Save to ini
  - Re-run analysis for the currently opened dump path
  - Refresh UI labels (tabs/columns/buttons)

**Step 2: Localize help/error MessageBox strings**

---

### Task 6: Packaging + docs + version bump

**Files:**
- Create: `dist/SkyrimDiagDumpTool.ini`
- Modify: `scripts/package.py` (include the new ini in the zip)
- Modify: `README.md` / `docs/README_KO.md` as needed
- Modify: `CHANGELOG.md`
- Modify: `CMakeLists.txt` + `vcpkg.json` (version bump)

**Step 1: Add release note**
- English default + Korean toggle

**Step 2: Windows build + zip**
- Sync WSL repo → Windows mirror
- Build: `scripts\\build-win.cmd`
- Package: `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger-v<ver>.zip`

