# Phase 2: 크래시 히스토리 상관분석 + 트러블슈팅 가이드 + 파서 테스트 보완

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 크래시 히스토리 bucket-key 상관분석으로 반복 패턴 감지, 크래시 유형별 단계별 트러블슈팅 가이드 제공, CrashLogger 파서 테스트 커버리지 보완

**Architecture:** CrashHistory에 bucket-level 통계 메서드 추가 → Analyzer에서 호출 → EvidenceBuilder에서 confidence 보정 + 트러블슈팅 가이드 매칭 → OutputWriter/WinUI 표시. 파서 테스트는 기존 18개 테스트에 엣지케이스 10+개 추가.

**Tech Stack:** C++20, nlohmann/json, WinUI 3 (C#/XAML), CMake, ctest

---

## Task 1: CrashHistory bucket-key 상관분석 메서드 (A1 핵심)

**Files:**
- Modify: `dump_tool/src/CrashHistory.h:29-44`
- Modify: `dump_tool/src/CrashHistory.cpp:86-142`
- Test: `tests/analysis_engine_runtime_tests.cpp:186-221`

**Step 1: Write the failing test**

`tests/analysis_engine_runtime_tests.cpp`에 다음 테스트 추가 (main 함수 위에):

```cpp
void TestCrashHistoryBucketCorrelation()
{
  CrashHistory history;

  // Add 3 entries with bucket-a
  for (int i = 0; i < 3; ++i) {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T0" + std::to_string(i) + ":00:00Z";
    e.dump_file = "dump_" + std::to_string(i) + ".dmp";
    e.bucket_key = "bucket-a";
    e.top_suspect = "modA.dll";
    e.all_suspects = { "modA.dll" };
    history.AddEntry(std::move(e));
  }

  // Add 1 entry with bucket-b
  {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T03:00:00Z";
    e.dump_file = "dump_3.dmp";
    e.bucket_key = "bucket-b";
    e.top_suspect = "modB.dll";
    e.all_suspects = { "modB.dll" };
    history.AddEntry(std::move(e));
  }

  // bucket-a should have 3 occurrences
  const auto corrA = history.GetBucketStats("bucket-a");
  assert(corrA.count == 3);
  assert(corrA.first_seen == "2026-02-23T00:00:00Z");
  assert(corrA.last_seen == "2026-02-23T02:00:00Z");

  // bucket-b should have 1 occurrence
  const auto corrB = history.GetBucketStats("bucket-b");
  assert(corrB.count == 1);

  // unknown bucket should have 0 occurrences
  const auto corrC = history.GetBucketStats("bucket-c");
  assert(corrC.count == 0);

  // empty bucket key should have 0 occurrences
  const auto corrEmpty = history.GetBucketStats("");
  assert(corrEmpty.count == 0);
}
```

main()에 `TestCrashHistoryBucketCorrelation();` 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R analysis_engine_runtime --output-on-failure`
Expected: FAIL (컴파일 에러 — `GetBucketStats` 미정의)

**Step 3: Write minimal implementation**

`dump_tool/src/CrashHistory.h`에 `BucketStats` 구조체와 메서드 선언 추가:

```cpp
struct BucketStats
{
  std::size_t count = 0;
  std::string first_seen;
  std::string last_seen;
};
```

CrashHistory 클래스 public에 추가:
```cpp
BucketStats GetBucketStats(const std::string& bucketKey) const;
```

`dump_tool/src/CrashHistory.cpp`에 구현 추가:

```cpp
BucketStats CrashHistory::GetBucketStats(const std::string& bucketKey) const
{
  BucketStats result{};
  if (bucketKey.empty()) {
    return result;
  }
  for (const auto& e : m_entries) {
    if (e.bucket_key == bucketKey) {
      result.count += 1;
      if (result.first_seen.empty() || e.timestamp_utc < result.first_seen) {
        result.first_seen = e.timestamp_utc;
      }
      if (result.last_seen.empty() || e.timestamp_utc > result.last_seen) {
        result.last_seen = e.timestamp_utc;
      }
    }
  }
  return result;
}
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R analysis_engine_runtime --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/src/CrashHistory.h dump_tool/src/CrashHistory.cpp tests/analysis_engine_runtime_tests.cpp
git commit -m "feat(A1): add CrashHistory::GetBucketStats for bucket-key correlation"
```

---

## Task 2: AnalysisResult에 history_correlation 필드 추가 + Analyzer 연동

**Files:**
- Modify: `dump_tool/src/Analyzer.h:95`
- Modify: `dump_tool/src/Analyzer.cpp:606-643`
- Test: guard test in `tests/crash_history_tests.cpp`

**Step 1: Write the failing test**

`tests/crash_history_tests.cpp`에 추가:

```cpp
static void TestAnalyzerHasHistoryCorrelationField()
{
  const auto header = ReadFile("dump_tool/src/Analyzer.h");
  assert(header.find("BucketCorrelation") != std::string::npos);
  assert(header.find("history_correlation") != std::string::npos);
}
```

main()에 `TestAnalyzerHasHistoryCorrelationField();` 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: FAIL (assert — 문자열 미발견)

**Step 3: Write minimal implementation**

`dump_tool/src/Analyzer.h`에 `history_stats` 뒤에 추가:

```cpp
struct BucketCorrelation
{
  std::size_t count = 0;
  std::string first_seen;
  std::string last_seen;
};
```

AnalysisResult에 필드 추가 (`history_stats` 줄 다음):
```cpp
BucketCorrelation history_correlation;
```

`dump_tool/src/Analyzer.cpp` line 641 (`out.history_stats = history.GetModuleStats(20);`) 뒤에 추가:

```cpp
      const auto bucketStats = history.GetBucketStats(WideToUtf8(out.crash_bucket_key));
      if (bucketStats.count > 1) {
        out.history_correlation.count = bucketStats.count;
        out.history_correlation.first_seen = bucketStats.first_seen;
        out.history_correlation.last_seen = bucketStats.last_seen;
      }
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp tests/crash_history_tests.cpp
git commit -m "feat(A1): add BucketCorrelation to AnalysisResult + Analyzer integration"
```

---

## Task 3: history_correlation을 Evidence에 반영 + confidence 보정

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp:478-506`
- Modify: `dump_tool/src/AnalyzerInternalsStackwalkScoring.cpp` (confidence 보정)
- Test: guard test in `tests/crash_history_tests.cpp`

**Step 1: Write the failing test**

`tests/crash_history_tests.cpp`에 추가:

```cpp
static void TestEvidenceHasCorrelationDisplay()
{
  const auto src = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
  assert(src.find("history_correlation") != std::string::npos);
  assert(src.find("bucket_key") != std::string::npos || src.find("same pattern") != std::string::npos || src.find("동일 패턴") != std::string::npos);
}
```

main()에 `TestEvidenceHasCorrelationDisplay();` 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: FAIL

**Step 3: Write minimal implementation**

`dump_tool/src/EvidenceBuilderInternalsEvidence.cpp`에서 기존 `history_stats` 블록(line 478) 뒤에 추가:

```cpp
  if (r.history_correlation.count > 1) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = ctx.en ? L"Repeated crash pattern" : L"반복 크래시 패턴";
    wchar_t buf[256]{};
    swprintf_s(buf,
      ctx.en ? L"Same bucket_key matched %zu times (first: %s)"
             : L"동일 패턴이 %zu회 발생 (최초: %s)",
      r.history_correlation.count,
      ToWideAscii(r.history_correlation.first_seen).c_str());
    e.details = buf;
    r.evidence.push_back(std::move(e));
  }
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/src/EvidenceBuilderInternalsEvidence.cpp tests/crash_history_tests.cpp
git commit -m "feat(A1): display bucket-key correlation as evidence item"
```

---

## Task 4: OutputWriter에 history_correlation JSON 출력

**Files:**
- Modify: `dump_tool/src/OutputWriter.cpp:270-281`
- Test: guard test in `tests/crash_history_tests.cpp`

**Step 1: Write the failing test**

`tests/crash_history_tests.cpp`에 추가:

```cpp
static void TestOutputWriterHasHistoryCorrelation()
{
  const auto src = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(src.find("history_correlation") != std::string::npos);
}
```

main()에 `TestOutputWriterHasHistoryCorrelation();` 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: FAIL

**Step 3: Write minimal implementation**

`dump_tool/src/OutputWriter.cpp`에서 `crash_history_stats` 블록(line 280) 뒤에 추가:

```cpp
  if (r.history_correlation.count > 1) {
    summary["history_correlation"] = {
      { "bucket_key", WideToUtf8(r.crash_bucket_key) },
      { "count", r.history_correlation.count },
      { "first_seen", r.history_correlation.first_seen },
      { "last_seen", r.history_correlation.last_seen },
    };
  }
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/src/OutputWriter.cpp tests/crash_history_tests.cpp
git commit -m "feat(A1): output history_correlation in summary JSON"
```

---

## Task 5: WinUI에 history_correlation 배지 표시

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs:6-8`
- Modify: `dump_tool_winui/MainWindow.xaml.cs:355-448`
- Modify: `dump_tool_winui/MainWindow.xaml`
- Test: `tests/winui_xaml_tests.cpp`

**Step 1: Write the failing test**

`tests/winui_xaml_tests.cpp`에 추가 (guard test):

```cpp
static void TestMainWindowHasCorrelationBadge()
{
  const auto xaml = ReadAllText("dump_tool_winui/MainWindow.xaml");
  assert(xaml.find("CorrelationBadge") != std::string::npos);

  const auto cs = ReadAllText("dump_tool_winui/MainWindow.xaml.cs");
  assert(cs.find("HistoryCorrelationCount") != std::string::npos || cs.find("history_correlation") != std::string::npos);

  const auto summary = ReadAllText("dump_tool_winui/AnalysisSummary.cs");
  assert(summary.find("HistoryCorrelationCount") != std::string::npos);
}
```

main()에 `TestMainWindowHasCorrelationBadge();` 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R winui_xaml --output-on-failure`
Expected: FAIL

**Step 3: Write minimal implementation**

**AnalysisSummary.cs** — 프로퍼티 추가 (line 8 뒤):
```csharp
public int HistoryCorrelationCount { get; init; }
```

LoadFromSummaryFile에서 읽기 추가 (return문 안, `ResourceItems` 뒤):
```csharp
HistoryCorrelationCount = root.TryGetProperty("history_correlation", out var histCorr)
    && histCorr.ValueKind == JsonValueKind.Object
    && histCorr.TryGetProperty("count", out var countNode)
    && countNode.TryGetInt32(out var count) ? count : 0,
```

**MainWindow.xaml** — BucketText 뒤에 배지 추가:
```xml
<TextBlock x:Name="CorrelationBadge" FontSize="13" Foreground="OrangeRed" Visibility="Collapsed" Margin="8,0,0,0" />
```

**MainWindow.xaml.cs** — RenderSummary에서 BucketText.Text 설정 뒤에 추가:
```csharp
if (summary.HistoryCorrelationCount > 1)
{
    CorrelationBadge.Text = _isKorean
        ? $"⚠ 동일 패턴 {summary.HistoryCorrelationCount}회 반복 발생"
        : $"⚠ Same pattern repeated {summary.HistoryCorrelationCount} times";
    CorrelationBadge.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
}
else
{
    CorrelationBadge.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
}
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R winui_xaml --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool_winui/AnalysisSummary.cs dump_tool_winui/MainWindow.xaml dump_tool_winui/MainWindow.xaml.cs tests/winui_xaml_tests.cpp
git commit -m "feat(A1): WinUI correlation badge for repeated crash patterns"
```

---

## Task 6: CrashLogger 파서 테스트 엣지케이스 보완 (C3)

**Files:**
- Modify: `tests/crashlogger_parser_tests.cpp`

**Step 1: Write the failing tests (edge cases)**

`tests/crashlogger_parser_tests.cpp`에 다음 테스트 함수들 추가:

```cpp
static void Test_LooksLikeCrashLogger_ProcessInfo()
{
  // "process info:" path 검증
  const std::string s =
    "CrashLoggerSSE v1.20.0\n"
    "PROCESS INFO:\n"
    "  SkyrimSE.exe version 1.6.1170\n";
  assert(LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_NotCrashLogger()
{
  const std::string s = "Some random log file\nWith multiple lines\n";
  assert(!LooksLikeCrashLoggerLogTextCore(s));
}

static void Test_LooksLikeCrashLogger_Empty()
{
  assert(!LooksLikeCrashLoggerLogTextCore(""));
}

static void Test_ParseCrashLoggerVersion_Missing()
{
  const std::string s = "Some random text\nNo version here\n";
  const auto ver = ParseCrashLoggerVersionAscii(s);
  assert(!ver);
}

static void Test_ParseCrashLoggerVersion_Empty()
{
  const auto ver = ParseCrashLoggerVersionAscii("");
  assert(!ver);
}

static void Test_ParseCppExceptionDetails_NoBlock()
{
  const std::string s =
    "CrashLoggerSSE v1.18.0\n"
    "CRASH TIME: 2026-02-23 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "  SomeMod.dll+0x111\n"
    "\n"
    "REGISTERS:\n";
  const auto ex = ParseCrashLoggerCppExceptionDetailsAscii(s);
  assert(!ex);
}

static void Test_ParseTopModules_EmptyInput()
{
  const auto mods = ParseCrashLoggerTopModulesAsciiLower("");
  assert(mods.empty());
}

static void Test_ParseTopModules_CrashLog_NoModules()
{
  const std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-01-31 12:34:56\n"
    "PROBABLE CALL STACK:\n"
    "WARNING: Stack trace capture failed - the call stack was likely corrupted.\n"
    "REGISTERS:\n";
  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.empty());
}

static void Test_ParseTopModules_ManyModules_CappedAt8()
{
  std::string s =
    "CrashLoggerSSE v1.17.0\n"
    "CRASH TIME: 2026-02-23 12:34:56\n"
    "PROBABLE CALL STACK:\n";
  for (int i = 0; i < 12; ++i) {
    s += "  mod" + std::to_string(i) + ".dll+0x100\n";
  }
  s += "REGISTERS:\n";
  const auto mods = ParseCrashLoggerTopModulesAsciiLower(s);
  assert(mods.size() <= 8);
}

static void Test_ParseCrashLoggerIni_HashComment()
{
  const std::string s =
    "# comment line\n"
    "[Debug]\n"
    "Crashlog Directory=C:\\\\Logs # inline comment\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(dir);
  assert(*dir == "C:\\\\Logs");
}

static void Test_ParseCrashLoggerIni_NoDebugSection()
{
  const std::string s =
    "[General]\n"
    "Crashlog Directory=C:\\\\Wrong\n";
  const auto dir = ParseCrashLoggerIniCrashlogDirectoryAscii(s);
  assert(!dir);
}
```

main()에 모든 새 테스트 호출 추가.

**Step 2: Run test to verify they pass (pure edge cases of existing code)**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crashlogger_parser --output-on-failure`
Expected: PASS (이 테스트들은 기존 코드의 엣지케이스를 확인하므로, 구현이 이미 올바르다면 바로 통과)

> 참고: C3은 코드 변경이 아닌 테스트 커버리지 보완이므로, TDD red-green이 아닌 "커버리지 확인" 패턴입니다.

**Step 3: Commit**

```bash
git add tests/crashlogger_parser_tests.cpp
git commit -m "test(C3): add 11 edge case tests for CrashLogger parser coverage"
```

---

## Task 7: TryExtractModulePlusOffsetTokenAscii 직접 테스트 (C3)

**Files:**
- Modify: `tests/crashlogger_parser_tests.cpp`

**Step 1: Write the tests**

```cpp
using skydiag::dump_tool::crashlogger_core::TryExtractModulePlusOffsetTokenAscii;

static void Test_TryExtractToken_ValidDll()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("  ExampleMod.dll+0x1234");
  assert(tok.has_value());
  assert(tok->find("ExampleMod.dll+") != std::string_view::npos);
}

static void Test_TryExtractToken_ValidExe()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("SkyrimSE.exe+0xABCD");
  assert(tok.has_value());
  assert(tok->find("SkyrimSE.exe+") != std::string_view::npos);
}

static void Test_TryExtractToken_NoModule()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("just some text");
  assert(!tok.has_value());
}

static void Test_TryExtractToken_Empty()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("");
  assert(!tok.has_value());
}

static void Test_TryExtractToken_NewFormat()
{
  const auto tok = TryExtractModulePlusOffsetTokenAscii("\t[ 0] 0x00007FF612345678 ExampleMod.dll+0000123\tmov eax, eax");
  assert(tok.has_value());
  assert(tok->find("ExampleMod.dll+") != std::string_view::npos);
}
```

main()에 호출 추가.

**Step 2: Run test to verify they pass**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crashlogger_parser --output-on-failure`
Expected: PASS

**Step 3: Commit**

```bash
git add tests/crashlogger_parser_tests.cpp
git commit -m "test(C3): add direct tests for TryExtractModulePlusOffsetTokenAscii"
```

---

## Task 8: IsSystemish/IsGameExe 헬퍼 직접 테스트 (C3)

**Files:**
- Modify: `tests/crashlogger_parser_tests.cpp`

**Step 1: Write the tests**

```cpp
using skydiag::dump_tool::crashlogger_core::IsSystemishModuleAsciiLower;
using skydiag::dump_tool::crashlogger_core::IsGameExeModuleAsciiLower;

static void Test_IsSystemish_KnownModules()
{
  assert(IsSystemishModuleAsciiLower("kernelbase.dll"));
  assert(IsSystemishModuleAsciiLower("ntdll.dll"));
  assert(IsSystemishModuleAsciiLower("kernel32.dll"));
  assert(IsSystemishModuleAsciiLower("ucrtbase.dll"));
  assert(IsSystemishModuleAsciiLower("user32.dll"));
  assert(IsSystemishModuleAsciiLower("win32u.dll"));
}

static void Test_IsSystemish_NotSystem()
{
  assert(!IsSystemishModuleAsciiLower("mymod.dll"));
  assert(!IsSystemishModuleAsciiLower("hdtsmp64.dll"));
  assert(!IsSystemishModuleAsciiLower(""));
}

static void Test_IsGameExe_KnownExes()
{
  assert(IsGameExeModuleAsciiLower("skyrimse.exe"));
  assert(IsGameExeModuleAsciiLower("skyrimae.exe"));
  assert(IsGameExeModuleAsciiLower("skyrimvr.exe"));
  assert(IsGameExeModuleAsciiLower("skyrim.exe"));
}

static void Test_IsGameExe_NotGameExe()
{
  assert(!IsGameExeModuleAsciiLower("mymod.dll"));
  assert(!IsGameExeModuleAsciiLower(""));
  assert(!IsGameExeModuleAsciiLower("skyrimse.dll"));
}
```

main()에 호출 추가.

**Step 2: Run test to verify they pass**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crashlogger_parser --output-on-failure`
Expected: PASS

**Step 3: Commit**

```bash
git add tests/crashlogger_parser_tests.cpp
git commit -m "test(C3): add direct tests for IsSystemishModule and IsGameExeModule helpers"
```

---

## Task 9: troubleshooting_guides.json 데이터 파일 생성 (B3)

**Files:**
- Create: `dump_tool/data/troubleshooting_guides.json`
- Test: guard test in existing `tests/json_schema_validation_guard_tests.cpp`

**Step 1: Write the failing test**

`tests/json_schema_validation_guard_tests.cpp`에 추가:

```cpp
static void TestTroubleshootingGuidesJsonHasVersionField()
{
  auto src = ReadAllText("dump_tool/data/troubleshooting_guides.json");
  AssertContains(src, "\"version\"", "troubleshooting_guides.json must have version field");
  AssertContains(src, "\"guides\"", "troubleshooting_guides.json must have guides array");
}
```

main()에 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R json_schema_validation --output-on-failure`
Expected: FAIL (파일 없음)

**Step 3: Create the data file**

```json
{
  "version": 1,
  "guides": [
    {
      "id": "ACCESS_VIOLATION",
      "match": { "exc_code": "0xC0000005" },
      "title_en": "Access Violation (0xC0000005)",
      "title_ko": "접근 위반 (0xC0000005)",
      "steps_en": [
        "Identify the fault module from the summary",
        "Check if the mod has a known update for your game version",
        "Reinstall the mod and verify SKSE/Address Library compatibility",
        "If the fault module is a hook framework DLL, check other suspect candidates first",
        "Disable the suspect mod, reproduce, and confirm the crash is gone"
      ],
      "steps_ko": [
        "요약에서 원인 모듈을 확인하세요",
        "해당 모드가 현재 게임 버전에 맞는 업데이트가 있는지 확인하세요",
        "모드를 재설치하고 SKSE/Address Library 호환성을 확인하세요",
        "원인 모듈이 훅 프레임워크 DLL이면 다른 의심 후보를 먼저 점검하세요",
        "의심 모드를 비활성화 후 재현해서 크래시가 사라지는지 확인하세요"
      ]
    },
    {
      "id": "D6DDDA_VRAM",
      "match": { "exc_code": "0xC0000005", "signature_id": "D6DDDA_VRAM" },
      "title_en": "VRAM Exhaustion (D6DDDA)",
      "title_ko": "VRAM 부족 (D6DDDA)",
      "steps_en": [
        "Check your GPU VRAM usage — this crash typically means VRAM is exhausted",
        "Lower texture quality in ENB/SkyrimPrefs.ini",
        "Reduce 4K texture mods or switch to 2K alternatives",
        "Close background apps using GPU (browsers, Discord overlay)",
        "If using ENB, try disabling post-processing effects one by one"
      ],
      "steps_ko": [
        "GPU VRAM 사용량을 확인하세요 — 이 크래시는 보통 VRAM 부족을 의미합니다",
        "ENB/SkyrimPrefs.ini에서 텍스처 품질을 낮추세요",
        "4K 텍스처 모드를 줄이거나 2K 대안으로 교체하세요",
        "GPU를 사용하는 백그라운드 앱(브라우저, Discord 오버레이)을 닫으세요",
        "ENB 사용 시 후처리 효과를 하나씩 꺼보세요"
      ]
    },
    {
      "id": "CPP_EXCEPTION",
      "match": { "exc_code": "0xE06D7363" },
      "title_en": "C++ Exception (0xE06D7363)",
      "title_ko": "C++ 예외 (0xE06D7363)",
      "steps_en": [
        "Confirm this was an actual CTD (not a handled exception false positive)",
        "Consider setting CrashHookMode=1 in SkyrimDiag.ini to filter handled exceptions",
        "Check the C++ Exception section for type and info details",
        "If DXGI_ERROR_DEVICE_REMOVED: likely a GPU driver crash — update GPU drivers",
        "If std::bad_alloc: RAM/VRAM exhaustion — reduce mod payload"
      ],
      "steps_ko": [
        "실제 CTD인지 확인하세요 (handled exception 오탐이 아닌지)",
        "SkyrimDiag.ini에서 CrashHookMode=1로 설정하여 handled exception을 필터링해 보세요",
        "C++ Exception 섹션에서 type과 info 세부 정보를 확인하세요",
        "DXGI_ERROR_DEVICE_REMOVED인 경우: GPU 드라이버 크래시 — 드라이버 업데이트",
        "std::bad_alloc인 경우: RAM/VRAM 부족 — 모드 부하를 줄이세요"
      ]
    },
    {
      "id": "FREEZE_HANG",
      "match": { "state_flags_contains": "hang" },
      "title_en": "Freeze / Infinite Loading",
      "title_ko": "프리징 / 무한 로딩",
      "steps_en": [
        "Check the WCT tab for deadlock indicators (isCycle=true)",
        "If deadlock: identify which DLLs hold the locks from the WCT thread chains",
        "If no deadlock: likely an infinite loop or busy wait — check the callstack for looping code",
        "Review the Events tab for what happened just before the freeze",
        "Disable recently added mods that affect scripting/animation/physics"
      ],
      "steps_ko": [
        "WCT 탭에서 데드락 표시(isCycle=true)를 확인하세요",
        "데드락인 경우: WCT 스레드 체인에서 어떤 DLL이 잠금을 보유하는지 확인하세요",
        "데드락이 아닌 경우: 무한 루프 또는 바쁜 대기일 가능성 — 콜스택에서 반복 코드 확인",
        "이벤트 탭에서 프리징 직전에 무엇이 일어났는지 확인하세요",
        "스크립팅/애니메이션/물리에 영향을 주는 최근 추가 모드를 비활성화하세요"
      ]
    },
    {
      "id": "LOADING_CRASH",
      "match": { "state_flags_contains": "loading" },
      "title_en": "Crash During Loading",
      "title_ko": "로딩 중 크래시",
      "steps_en": [
        "Loading crashes often involve animation/mesh/texture/skeleton/script initialization",
        "Check the Resources tab for recently loaded .nif/.hkx/.tri files",
        "If a specific resource appears suspicious, identify its providing mod",
        "Check for asset conflicts in MO2 (multiple mods providing the same file)",
        "Disable animation/skeleton/body/physics/precaching mods one by one"
      ],
      "steps_ko": [
        "로딩 중 크래시는 보통 애니메이션/메쉬/텍스처/스켈레톤/스크립트 초기화와 관련됩니다",
        "리소스 탭에서 최근 로드된 .nif/.hkx/.tri 파일을 확인하세요",
        "의심되는 리소스가 있으면 제공 모드를 확인하세요",
        "MO2에서 에셋 충돌(여러 모드가 같은 파일 제공)을 확인하세요",
        "애니메이션/스켈레톤/바디/물리/프리캐시 모드를 하나씩 비활성화하세요"
      ]
    },
    {
      "id": "SNAPSHOT",
      "match": { "state_flags_contains": "snapshot" },
      "title_en": "Snapshot Dump (Not a Crash)",
      "title_ko": "스냅샷 덤프 (크래시 아님)",
      "steps_en": [
        "This dump was captured without an actual crash/hang",
        "It is useful for state inspection but NOT for identifying crash causes",
        "To diagnose a real issue, capture during an actual CTD or freeze",
        "For freezes: use Ctrl+Shift+F12 or wait for auto hang detection"
      ],
      "steps_ko": [
        "이 덤프는 실제 크래시/행 없이 캡처되었습니다",
        "상태 검사에는 유용하지만 크래시 원인을 판별하기에는 부적합합니다",
        "실제 문제를 진단하려면 실제 CTD 또는 프리징 중에 캡처하세요",
        "프리징의 경우: Ctrl+Shift+F12를 사용하거나 자동 행 감지를 기다리세요"
      ]
    }
  ]
}
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R json_schema_validation --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/data/troubleshooting_guides.json tests/json_schema_validation_guard_tests.cpp
git commit -m "feat(B3): create troubleshooting_guides.json with 6 crash-type guides"
```

---

## Task 10: 트러블슈팅 가이드 매칭 로직 + Recommendations 통합 (B3)

**Files:**
- Modify: `dump_tool/src/Analyzer.h:125-127` (troubleshooting_steps 필드)
- Modify: `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Test: guard test

**Step 1: Write the failing test**

`tests/crash_history_tests.cpp`에 추가 (이름이 좀 벗어나지만 가장 적절한 guard test 파일):

```cpp
static void TestRecommendationsHasTroubleshootingGuide()
{
  const auto rec = ReadFile("dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp");
  assert(rec.find("troubleshooting") != std::string::npos || rec.find("Troubleshooting") != std::string::npos);

  const auto header = ReadFile("dump_tool/src/Analyzer.h");
  assert(header.find("troubleshooting_steps") != std::string::npos);

  const auto writer = ReadFile("dump_tool/src/OutputWriter.cpp");
  assert(writer.find("troubleshooting_steps") != std::string::npos);
}
```

main()에 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: FAIL

**Step 3: Write minimal implementation**

**Analyzer.h** — AnalysisResult에 `recommendations` 뒤에 추가:
```cpp
std::vector<std::wstring> troubleshooting_steps;
std::wstring troubleshooting_title;
```

**EvidenceBuilderInternalsRecommendations.cpp** — BuildRecommendations 함수 끝(closing brace 전)에 추가:

```cpp
  // Best-effort troubleshooting guide matching (loaded from data/troubleshooting_guides.json).
  // Note: The actual JSON loading happens in Analyzer.cpp; here we add a placeholder
  // recommendation pointing to the steps if they exist.
  if (!r.troubleshooting_steps.empty()) {
    r.recommendations.push_back(en
      ? (L"[Troubleshooting] See the troubleshooting checklist (" + std::to_wstring(r.troubleshooting_steps.size()) + L" steps) for this crash type.")
      : (L"[트러블슈팅] 이 크래시 유형에 대한 단계별 체크리스트(" + std::to_wstring(r.troubleshooting_steps.size()) + L"단계)를 확인하세요."));
  }
```

**OutputWriter.cpp** — `crash_history_stats` 블록 뒤 (또는 `history_correlation` 뒤)에 추가:

```cpp
  if (!r.troubleshooting_steps.empty()) {
    auto steps = nlohmann::json::array();
    for (const auto& step : r.troubleshooting_steps) {
      steps.push_back(WideToUtf8(step));
    }
    summary["troubleshooting_steps"] = {
      { "title", WideToUtf8(r.troubleshooting_title) },
      { "steps", std::move(steps) },
    };
  }
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R crash_history_tests --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool/src/Analyzer.h dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp dump_tool/src/OutputWriter.cpp tests/crash_history_tests.cpp
git commit -m "feat(B3): add troubleshooting_steps field + recommendation + JSON output"
```

---

## Task 11: Analyzer에서 troubleshooting_guides.json 로드 + 매칭 (B3)

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp` (BuildEvidenceAndSummary 호출 전)
- Test: `tests/analysis_engine_runtime_tests.cpp`

**Step 1: Write the failing test**

`tests/analysis_engine_runtime_tests.cpp`에 추가:

```cpp
void TestTroubleshootingGuideLoading()
{
  // Verify the guides file exists and can be parsed
  const auto guidesPath = ProjectRoot() / "dump_tool" / "data" / "troubleshooting_guides.json";
  std::ifstream f(guidesPath);
  assert(f.is_open());
  const auto j = nlohmann::json::parse(f);
  assert(j.contains("version"));
  assert(j.contains("guides"));
  assert(j["guides"].is_array());
  assert(j["guides"].size() >= 5);

  // Verify ACCESS_VIOLATION guide exists
  bool foundAV = false;
  for (const auto& g : j["guides"]) {
    if (g.value("id", "") == "ACCESS_VIOLATION") {
      foundAV = true;
      assert(g.contains("steps_en"));
      assert(g.contains("steps_ko"));
      assert(g["steps_en"].size() >= 3);
    }
  }
  assert(foundAV);
}
```

main()에 `TestTroubleshootingGuideLoading();` 호출 추가.

**Step 2: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R analysis_engine_runtime --output-on-failure`
Expected: PASS (데이터 파일은 Task 9에서 이미 생성)

**Step 3: Analyzer.cpp에 매칭 로직 추가**

`dump_tool/src/Analyzer.cpp`에서 `BuildEvidenceAndSummary(out, opt.language);` 호출 직전 (line 645)에 추가:

```cpp
  // Best-effort troubleshooting guide matching.
  if (!opt.data_dir.empty()) {
    const auto guidesPath = std::filesystem::path(opt.data_dir) / L"troubleshooting_guides.json";
    try {
      std::ifstream gf(guidesPath);
      if (gf.is_open()) {
        const auto gj = nlohmann::json::parse(gf, nullptr, true);
        if (gj.contains("guides") && gj["guides"].is_array()) {
          const bool en = (opt.language == i18n::Language::kEnglish);
          const std::string excHex = [&]() {
            char buf[32]{};
            if (out.exc_code != 0) {
              snprintf(buf, sizeof(buf), "0x%08X", out.exc_code);
            }
            return std::string(buf);
          }();
          const std::string sigId = out.signature_match ? out.signature_match->id : "";
          const bool isHang = (out.state_flags & skydiag::kState_Hang) != 0u;
          const bool isLoading = (out.state_flags & skydiag::kState_Loading) != 0u;
          const bool isSnapshot = (out.state_flags & skydiag::kState_Snapshot) != 0u;

          for (const auto& guide : gj["guides"]) {
            if (!guide.is_object() || !guide.contains("match")) continue;
            const auto& match = guide["match"];
            bool matched = true;

            if (match.contains("exc_code")) {
              const auto req = match.value("exc_code", "");
              if (req != excHex) { matched = false; }
            }
            if (matched && match.contains("signature_id")) {
              const auto req = match.value("signature_id", "");
              if (req != sigId) { matched = false; }
            }
            if (matched && match.contains("state_flags_contains")) {
              const auto req = match.value("state_flags_contains", "");
              if (req == "hang" && !isHang) { matched = false; }
              if (req == "loading" && !isLoading) { matched = false; }
              if (req == "snapshot" && !isSnapshot) { matched = false; }
            }

            if (matched) {
              const std::string titleKey = en ? "title_en" : "title_ko";
              const std::string stepsKey = en ? "steps_en" : "steps_ko";
              out.troubleshooting_title = Utf8ToWide(guide.value(titleKey, guide.value("title_en", "")));
              if (guide.contains(stepsKey) && guide[stepsKey].is_array()) {
                for (const auto& step : guide[stepsKey]) {
                  if (step.is_string()) {
                    out.troubleshooting_steps.push_back(Utf8ToWide(step.get<std::string>()));
                  }
                }
              }
              break;  // first match wins (most specific guides should be first in JSON)
            }
          }
        }
      }
    } catch (...) {
      // Troubleshooting guides are best-effort; silently skip on failure.
    }
  }
```

> 참고: `Utf8ToWide` 함수가 없으면 `dump_tool/src/Utf.h`에 이미 `skydiag::dump_tool::Utf8ToWide`가 있는지 확인하고, 없으면 간단한 `std::wstring` 변환 사용. `ToWideAscii`를 사용해도 됨 (ASCII 범위만 다루므로 충분).

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS

**Step 5: Commit**

```bash
git add dump_tool/src/Analyzer.cpp tests/analysis_engine_runtime_tests.cpp
git commit -m "feat(B3): load and match troubleshooting guides in Analyzer"
```

---

## Task 12: WinUI에 트러블슈팅 체크리스트 표시 (B3)

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs`
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`
- Test: `tests/winui_xaml_tests.cpp`

**Step 1: Write the failing test**

`tests/winui_xaml_tests.cpp`에 추가:

```cpp
static void TestMainWindowHasTroubleshootingSection()
{
  const auto xaml = ReadAllText("dump_tool_winui/MainWindow.xaml");
  assert(xaml.find("TroubleshootingExpander") != std::string::npos || xaml.find("Troubleshooting") != std::string::npos);

  const auto cs = ReadAllText("dump_tool_winui/MainWindow.xaml.cs");
  assert(cs.find("TroubleshootingSteps") != std::string::npos || cs.find("troubleshooting_steps") != std::string::npos);

  const auto summary = ReadAllText("dump_tool_winui/AnalysisSummary.cs");
  assert(summary.find("TroubleshootingSteps") != std::string::npos);
}
```

main()에 호출 추가.

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R winui_xaml --output-on-failure`
Expected: FAIL

**Step 3: Write minimal implementation**

**AnalysisSummary.cs** — 프로퍼티 추가:
```csharp
public string TroubleshootingTitle { get; init; } = string.Empty;
public IReadOnlyList<string> TroubleshootingSteps { get; init; } = Array.Empty<string>();
```

LoadFromSummaryFile return문 안에 추가:
```csharp
TroubleshootingTitle = root.TryGetProperty("troubleshooting_steps", out var tsNode)
    && tsNode.ValueKind == JsonValueKind.Object
    ? ReadString(tsNode, "title") : string.Empty,
TroubleshootingSteps = root.TryGetProperty("troubleshooting_steps", out var tsNode2)
    && tsNode2.ValueKind == JsonValueKind.Object
    && tsNode2.TryGetProperty("steps", out var stepsNode)
    && stepsNode.ValueKind == JsonValueKind.Array
    ? stepsNode.EnumerateArray()
        .Where(s => s.ValueKind == JsonValueKind.String)
        .Select(s => s.GetString() ?? string.Empty)
        .Where(s => !string.IsNullOrWhiteSpace(s))
        .ToList()
    : new List<string>(),
```

**MainWindow.xaml** — Recommendations ListView 아래에 추가:
```xml
<Expander x:Name="TroubleshootingExpander" Header="Troubleshooting" IsExpanded="False" Visibility="Collapsed" Margin="0,8,0,0" HorizontalAlignment="Stretch">
    <ItemsControl x:Name="TroubleshootingList" Margin="8,4">
        <ItemsControl.ItemTemplate>
            <DataTemplate>
                <TextBlock Text="{Binding}" TextWrapping="Wrap" Margin="0,2" />
            </DataTemplate>
        </ItemsControl.ItemTemplate>
    </ItemsControl>
</Expander>
```

**MainWindow.xaml.cs** — RenderSummary에서 recommendations 처리 뒤에 추가:
```csharp
if (summary.TroubleshootingSteps.Count > 0)
{
    TroubleshootingExpander.Header = string.IsNullOrWhiteSpace(summary.TroubleshootingTitle)
        ? T("Troubleshooting", "트러블슈팅 가이드")
        : summary.TroubleshootingTitle;
    TroubleshootingExpander.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
    var numberedSteps = summary.TroubleshootingSteps
        .Select((step, i) => $"{i + 1}. {step}")
        .ToList();
    TroubleshootingList.ItemsSource = numberedSteps;
}
else
{
    TroubleshootingExpander.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
}
```

**Step 4: Run test to verify it passes**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R winui_xaml --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add dump_tool_winui/AnalysisSummary.cs dump_tool_winui/MainWindow.xaml dump_tool_winui/MainWindow.xaml.cs tests/winui_xaml_tests.cpp
git commit -m "feat(B3): WinUI collapsible troubleshooting checklist"
```

---

## Task 13: 전체 테스트 통과 확인 + 최종 정리

**Step 1: Run all tests**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS

**Step 2: Verify test count increased**

Count should be 40+ (기존 40 + 신규 테스트)

**Step 3: Final commit if any cleanup needed**

```bash
git add -A
git status
```

---

## 실행 순서 요약

```
Task  1: CrashHistory.GetBucketStats 구현 + 테스트        [A1]
Task  2: AnalysisResult.history_correlation + Analyzer     [A1]
Task  3: Evidence에 correlation 표시                       [A1]
Task  4: OutputWriter에 history_correlation JSON           [A1]
Task  5: WinUI correlation badge                           [A1]
Task  6: CrashLogger 파서 엣지케이스 테스트 11개           [C3]
Task  7: TryExtractModulePlusOffsetToken 직접 테스트       [C3]
Task  8: IsSystemish/IsGameExe 직접 테스트                 [C3]
Task  9: troubleshooting_guides.json 생성                  [B3]
Task 10: troubleshooting_steps 필드 + Recommendations      [B3]
Task 11: Analyzer에서 가이드 로드 + 매칭                   [B3]
Task 12: WinUI 트러블슈팅 체크리스트                       [B3]
Task 13: 전체 테스트 + 정리                                [Final]
```
