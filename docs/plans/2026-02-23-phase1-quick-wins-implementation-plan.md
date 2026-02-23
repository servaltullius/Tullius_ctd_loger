# Phase 1 Quick Wins Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Dual namespace 통합, JSON 스키마 검증, Preflight 확장, 커뮤니티 공유 포맷 — 4개 Quick Win을 TDD로 구현한다.

**Architecture:** DumpTool의 `internal::IsKnownHookFramework` 래퍼를 제거하고 `minidump::` 단일 구현으로 통합. JSON 로더들에 version/구조 검증을 추가. Helper Preflight에 플러그인 수/버전 체크를 확장. WinUI에 커뮤니티 공유 포맷 버튼을 추가.

**Tech Stack:** C++20, nlohmann/json, WinUI 3 (C#), CMake/CTest

---

## Task 1: C4 — Dual Namespace `IsKnownHookFramework` 통합

### 배경
`internal::IsKnownHookFramework` (EvidenceBuilderInternalsUtil.cpp:131-134)는 `minidump::IsKnownHookFramework`를 단순 호출하는 래퍼. `EvidenceBuilderInternalsPriv.h:69`에 선언, 3개 파일에서 사용. 래퍼를 제거하고 호출처에서 `minidump::` 직접 사용으로 전환.

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderInternalsPriv.h:69` — `IsKnownHookFramework` 선언 제거
- Modify: `dump_tool/src/EvidenceBuilderInternalsUtil.cpp:131-134` — 래퍼 함수 제거
- Modify: `dump_tool/src/EvidenceBuilderInternals.cpp:58` — `minidump::IsKnownHookFramework` 직접 호출
- Modify: `dump_tool/src/EvidenceBuilderInternalsSummary.cpp:30,66` — `minidump::IsKnownHookFramework` 직접 호출
- Modify: `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp:24,38` — `minidump::IsKnownHookFramework` 직접 호출
- Test: `tests/hook_framework_guard_tests.cpp` — 기존 가드 통과 확인

**Step 1: 기존 테스트가 통과하는지 확인**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R hook_framework --output-on-failure`
Expected: PASS

**Step 2: `EvidenceBuilderInternalsPriv.h`에서 선언 제거**

`dump_tool/src/EvidenceBuilderInternalsPriv.h:69`의 `bool IsKnownHookFramework(std::wstring_view filename);` 줄을 삭제한다.

**Step 3: `EvidenceBuilderInternalsUtil.cpp`에서 래퍼 함수 제거**

`dump_tool/src/EvidenceBuilderInternalsUtil.cpp:131-134`의 `IsKnownHookFramework` 함수 전체를 삭제한다.

**Step 4: `EvidenceBuilderInternals.cpp`에서 직접 호출로 전환**

`dump_tool/src/EvidenceBuilderInternals.cpp:58`의 `IsKnownHookFramework(...)` 호출을 `minidump::IsKnownHookFramework(...)` 으로 변경한다. 파일 상단에 `#include "MinidumpUtil.h"`가 이미 있는지 확인하고, 없으면 추가.

**Step 5: `EvidenceBuilderInternalsSummary.cpp`에서 직접 호출로 전환**

`dump_tool/src/EvidenceBuilderInternalsSummary.cpp:30,66`의 `IsKnownHookFramework(...)` 호출을 `minidump::IsKnownHookFramework(...)` 으로 변경한다. `#include "MinidumpUtil.h"`가 필요하면 추가.

**Step 6: `EvidenceBuilderInternalsRecommendations.cpp`에서 직접 호출로 전환**

`dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp:24,38`의 `IsKnownHookFramework(...)` 호출을 `minidump::IsKnownHookFramework(...)` 으로 변경한다. `#include "MinidumpUtil.h"`가 필요하면 추가.

**Step 7: 빌드 및 테스트 실행**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS (39/39 이상)

**Step 8: `IsSystemishModule`, `IsLikelyWindowsSystemModulePath`, `IsGameExeModule` 래퍼도 동일 패턴 적용**

`EvidenceBuilderInternalsUtil.cpp:115-128`의 3개 래퍼도 동일하게 제거하고, `EvidenceBuilderInternalsPriv.h:66-68`의 선언을 삭제한다. 호출처에서 `minidump::` 접두사로 직접 호출하도록 전환. `IsGameExeModule`은 `EvidenceBuilderInternalsUtil.cpp:125-129`에 자체 구현이 있으므로 동작이 동일한지 확인 후 전환 (minidump:: 버전은 `MinidumpUtil.h:46`에 있음).

**Step 9: 빌드 및 전체 테스트**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS

**Step 10: 커밋**

```bash
git add dump_tool/src/EvidenceBuilderInternalsPriv.h dump_tool/src/EvidenceBuilderInternalsUtil.cpp dump_tool/src/EvidenceBuilderInternals.cpp dump_tool/src/EvidenceBuilderInternalsSummary.cpp dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp
git commit -m "refactor: unify dual namespace — remove internal:: wrappers for minidump:: functions"
```

---

## Task 2: C2 — JSON 데이터 파일 스키마 검증

### 배경
4개 JSON 데이터 파일(`hook_frameworks.json`, `crash_signatures.json`, `plugin_rules.json`, `graphics_injection_rules.json`)의 로더에 version 필드 검증과 항목별 필수 키 검증을 추가한다. `crash_signatures.json` 로더(`SignatureDatabase::LoadFromJson`)가 이미 가장 견고한 검증 패턴을 가지고 있음 — 이 패턴을 나머지에 확산.

**Files:**
- Modify: `dump_tool/src/MinidumpUtil.cpp:355-395` — `LoadHookFrameworksFromJson`에 version 검증 추가
- Modify: `dump_tool/src/PluginRules.cpp:220-304` — `PluginRules::LoadFromJson`에 version 검증 추가
- Modify: `dump_tool/src/SignatureDatabase.cpp:116-223` — version 검증 추가 (이미 항목별 검증 있음)
- Test: `tests/hook_framework_json_tests.cpp` — version 검증 가드 추가
- Test: `tests/json_schema_validation_guard_tests.cpp` — 신규 가드 테스트
- Modify: `tests/CMakeLists.txt` — 신규 테스트 타깃 등록

**Step 1: 가드 테스트 작성 — `tests/json_schema_validation_guard_tests.cpp`**

```cpp
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  // All JSON data files must have a "version" field
  const auto hookFw = ReadAllText(repoRoot / "dump_tool" / "data" / "hook_frameworks.json");
  const auto sigs = ReadAllText(repoRoot / "dump_tool" / "data" / "crash_signatures.json");
  const auto rules = ReadAllText(repoRoot / "dump_tool" / "data" / "plugin_rules.json");
  const auto gfx = ReadAllText(repoRoot / "dump_tool" / "data" / "graphics_injection_rules.json");

  AssertContains(hookFw, "\"version\"", "hook_frameworks.json must have a version field.");
  AssertContains(sigs, "\"version\"", "crash_signatures.json must have a version field.");
  AssertContains(rules, "\"version\"", "plugin_rules.json must have a version field.");
  AssertContains(gfx, "\"version\"", "graphics_injection_rules.json must have a version field.");

  // Loaders must check version field
  const auto minidumpUtil = ReadAllText(repoRoot / "dump_tool" / "src" / "MinidumpUtil.cpp");
  const auto pluginRules = ReadAllText(repoRoot / "dump_tool" / "src" / "PluginRules.cpp");
  const auto sigDb = ReadAllText(repoRoot / "dump_tool" / "src" / "SignatureDatabase.cpp");

  AssertContains(minidumpUtil, "\"version\"", "LoadHookFrameworksFromJson must validate version field.");
  AssertContains(pluginRules, "\"version\"", "PluginRules::LoadFromJson must validate version field.");
  AssertContains(sigDb, "\"version\"", "SignatureDatabase::LoadFromJson must validate version field.");

  return 0;
}
```

**Step 2: `tests/CMakeLists.txt`에 테스트 타깃 등록**

기존 `skydiag_hook_framework_guard_tests` 블록(라인 167-177) 뒤에 추가:

```cmake
add_executable(skydiag_json_schema_validation_guard_tests
  json_schema_validation_guard_tests.cpp
)

set_target_properties(skydiag_json_schema_validation_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_json_schema_validation_guard_tests COMMAND skydiag_json_schema_validation_guard_tests)
```

**Step 3: 테스트가 실패하는지 확인**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R json_schema_validation --output-on-failure`
Expected: FAIL (SignatureDatabase.cpp에 "version" 문자열이 없으므로)

**Step 4: `LoadHookFrameworksFromJson`에 version 검증 추가**

`dump_tool/src/MinidumpUtil.cpp` — `LoadHookFrameworksFromJson` 함수 내 `j.contains("frameworks")` 체크 바로 뒤에 version 검증 추가:

```cpp
// 현재 (라인 368-369):
if (!j.is_object() || !j.contains("frameworks") || !j["frameworks"].is_array()) {
  return;
}

// 변경 후:
if (!j.is_object() || !j.contains("frameworks") || !j["frameworks"].is_array()) {
  return;
}
if (!j.contains("version") || !j["version"].is_number_unsigned()) {
  return;  // Reject files without version tag.
}
```

**Step 5: `PluginRules::LoadFromJson`에 version 검증 추가**

`dump_tool/src/PluginRules.cpp:228`의 기존 체크 뒤에 추가:

```cpp
// 현재 (라인 228):
if (!j.is_object() || !j.contains("rules") || !j["rules"].is_array()) {
  return false;
}

// 변경 후:
if (!j.is_object() || !j.contains("rules") || !j["rules"].is_array()) {
  return false;
}
if (!j.contains("version") || !j["version"].is_number_unsigned()) {
  return false;
}
```

**Step 6: `SignatureDatabase::LoadFromJson`에 version 검증 추가**

`dump_tool/src/SignatureDatabase.cpp:124`의 기존 체크 뒤에 추가:

```cpp
// 현재 (라인 124):
if (!j.is_object() || !j.contains("signatures") || !j["signatures"].is_array()) {
  return false;
}

// 변경 후:
if (!j.is_object() || !j.contains("signatures") || !j["signatures"].is_array()) {
  return false;
}
if (!j.contains("version") || !j["version"].is_number_unsigned()) {
  return false;
}
```

**Step 7: 빌드 및 테스트**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS

**Step 8: 커밋**

```bash
git add dump_tool/src/MinidumpUtil.cpp dump_tool/src/PluginRules.cpp dump_tool/src/SignatureDatabase.cpp tests/json_schema_validation_guard_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add version field validation to all JSON data file loaders"
```

---

## Task 3: A4 — Preflight 환경 검증 확장

### 배경
`CompatibilityPreflight.cpp`에 2개 체크를 추가: (1) 비-ESL 일반 플러그인 254개 초과 경고, (2) 알려진 비호환 모드 조합 경고. SKSE↔Skyrim 버전 불일치는 SKSE 버전을 런타임에서 추출하기 어려우므로(별도 DLL 버전 쿼리 필요) `plugin_rules.json` 기반 간접 감지로 대체.

**Files:**
- Modify: `helper/src/CompatibilityPreflight.cpp:243-293` — 2개 체크 추가
- Modify: `dump_tool/data/plugin_rules.json` — `ESP_FULL_SLOT_NEAR_LIMIT` 규칙 추가
- Test: `tests/helper_preflight_guard_tests.cpp` — 새 체크 가드 추가

**Step 1: 가드 테스트 확장**

`tests/helper_preflight_guard_tests.cpp`에 새 assertion 추가:

```cpp
// 기존 assertion 뒤에 추가:
AssertContains(
  preflightCppText,
  "FULL_PLUGIN_SLOT_LIMIT",
  "Preflight must warn when non-ESL plugin count approaches 254 limit.");
AssertContains(
  preflightCppText,
  "KNOWN_INCOMPATIBLE_COMBO",
  "Preflight must check known incompatible mod combinations.");
```

**Step 2: 테스트가 실패하는지 확인**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R preflight_guard --output-on-failure`
Expected: FAIL

**Step 3: `CompatibilityPreflight.cpp`에 non-ESL 플러그인 수 체크 추가**

`helper/src/CompatibilityPreflight.cpp` — 기존 `SYMBOL_POLICY` 체크(라인 282-292) 뒤에 추가:

```cpp
  // Non-ESL (full) plugin slot limit check.
  {
    std::size_t fullPluginCount = 0;
    for (const auto& p : pluginScan.plugins) {
      if (p.is_active && !p.is_esl) {
        ++fullPluginCount;
      }
    }
    const bool nearLimit = (fullPluginCount >= 240);
    checks.push_back(PreflightCheck{
      "FULL_PLUGIN_SLOT_LIMIT",
      nearLimit ? "warn" : "ok",
      nearLimit ? "high" : "low",
      nearLimit
        ? ("비-ESL 플러그인 " + std::to_string(fullPluginCount) + "개 — 254 슬롯 한계에 근접합니다. CTD 위험이 높아집니다.")
        : ("비-ESL 플러그인 " + std::to_string(fullPluginCount) + "개 — 슬롯 여유 있음."),
      nearLimit
        ? ("Non-ESL plugin count " + std::to_string(fullPluginCount) + " — approaching 254 slot limit. High CTD risk.")
        : ("Non-ESL plugin count " + std::to_string(fullPluginCount) + " — within safe range."),
    });
  }
```

**Step 4: 알려진 비호환 모드 조합 체크 추가**

같은 위치에 이어서 추가:

```cpp
  // Known incompatible mod combinations.
  {
    const bool hasCrashLoggerAndTrainwreck = hasCrashLoggerBinary && hasTrainwreckBinary;
    // Already covered by CRASH_LOGGER_CONFLICT — add more combos here.
    const bool hasMultiplePhysics =
      HasModuleAny(moduleNames, { L"hdtssephysics.dll" }) &&
      HasModuleAny(moduleNames, { L"hdtsmp64.dll" });
    const bool hasIncompatCombo = hasMultiplePhysics;
    std::string detailKo;
    std::string detailEn;
    if (hasMultiplePhysics) {
      detailKo = "HDT-SMP와 HDT-SMP (Faster) 물리 모드가 동시 로드 — 충돌 가능성이 높습니다.";
      detailEn = "HDT-SMP and HDT-SMP (Faster) physics mods loaded simultaneously — likely conflict.";
    }
    checks.push_back(PreflightCheck{
      "KNOWN_INCOMPATIBLE_COMBO",
      hasIncompatCombo ? "warn" : "ok",
      hasIncompatCombo ? "high" : "low",
      hasIncompatCombo ? detailKo : "알려진 비호환 모드 조합이 감지되지 않았습니다.",
      hasIncompatCombo ? detailEn : "No known incompatible mod combinations detected.",
    });
  }
```

**Step 5: `plugin_rules.json`에 `ESP_FULL_SLOT_NEAR_LIMIT` 규칙 추가**

`dump_tool/data/plugin_rules.json`의 `rules` 배열 끝에 추가:

```json
    ,
    {
      "id": "ESP_FULL_SLOT_NEAR_LIMIT",
      "condition": {
        "full_plugin_count_gte": 240
      },
      "diagnosis": {
        "cause_ko": "비-ESL 플러그인 수가 슬롯 한계(254)에 근접 — 크래시 위험 높음",
        "cause_en": "Non-ESL plugin count approaching slot limit (254) — high crash risk",
        "confidence": "high",
        "recommendations_ko": [
          "[필수] 불필요한 ESP를 ESL 플래그로 변환하거나 제거하세요",
          "[도구] SSEEdit의 'Compact FormIDs for ESL' 기능을 활용하세요"
        ],
        "recommendations_en": [
          "[Required] Convert unnecessary ESPs to ESL-flagged or remove them",
          "[Tool] Use SSEEdit's 'Compact FormIDs for ESL' feature"
        ]
      }
    }
```

참고: `PluginRules::Evaluate()`에서 `full_plugin_count_gte` 조건을 처리하려면 `PluginRules.cpp`에 조건 파싱/평가 로직도 추가해야 하지만, Preflight에서 이미 직접 계산하므로 **DumpTool 쪽은 Phase 2에서 처리**한다. 현재는 JSON 데이터만 준비.

**Step 6: 빌드 및 테스트**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS

**Step 7: 커밋**

```bash
git add helper/src/CompatibilityPreflight.cpp dump_tool/data/plugin_rules.json tests/helper_preflight_guard_tests.cpp
git commit -m "feat: extend preflight — non-ESL slot limit warning and incompatible mod combo check"
```

---

## Task 4: B1 — Discord/Reddit 커뮤니티 공유용 요약 복사

### 배경
`MainWindow.xaml.cs`의 `BuildSummaryClipboardText()`(라인 470-509)가 플레인 텍스트를 생성. 별도의 마크다운+이모지 포맷 함수를 추가하고, "커뮤니티 공유" 버튼을 WinUI에 배치.

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml.cs:470-509` — `BuildCommunityShareText()` 함수 추가
- Modify: `dump_tool_winui/MainWindow.xaml.cs` — `CopyShareButton_Click` 핸들러 추가
- Modify: `dump_tool_winui/MainWindow.xaml` — 공유 버튼 XAML 추가
- Test: `tests/winui_xaml_tests.cpp` — 공유 버튼 가드 추가

**Step 1: XAML 가드 테스트 확장**

`tests/winui_xaml_tests.cpp`에 기존 assertion 뒤에 추가:

```cpp
assert(xaml.find("CopyShareButton") != std::string::npos && "Community share copy button missing in XAML");
assert(xaml.find("CopyShareButton_Click") != std::string::npos && "Community share click handler not wired in XAML");
```

**Step 2: 테스트가 실패하는지 확인**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R winui_xaml --output-on-failure`
Expected: FAIL

**Step 3: `MainWindow.xaml`에 공유 버튼 추가**

`dump_tool_winui/MainWindow.xaml` — 기존 `CopySummaryButton` (라인 434-440) 뒤에 추가:

```xml
<Button x:Name="CopyShareButton"
        Grid.Column="2"
        Click="CopyShareButton_Click"
        VerticalAlignment="Center"
        Padding="14,8"
        FontSize="13"
        Content="&#x1F4CB; Share" />
```

참고: Grid.Column 배치는 기존 레이아웃에 맞게 조정이 필요할 수 있음. `CopySummaryButton`이 `Grid.Column="1"`이므로, 새 버튼은 컬럼 추가 또는 StackPanel 배치로 해결.

**Step 4: `MainWindow.xaml.cs`에 `BuildCommunityShareText()` 함수 추가**

`BuildSummaryClipboardText()` (라인 509) 뒤에 추가:

```csharp
private string? BuildCommunityShareText()
{
    var summary = _currentSummary;
    if (summary is null)
    {
        return null;
    }

    var lines = new List<string>();

    // Header
    lines.Add(_isKorean
        ? "🔴 Skyrim CTD 리포트 — SkyrimDiag"
        : "🔴 Skyrim CTD Report — SkyrimDiag");

    // Primary suspect
    if (summary.Suspects.Count > 0)
    {
        var top = summary.Suspects[0];
        var conf = !string.IsNullOrWhiteSpace(top.Confidence) ? top.Confidence : "?";
        lines.Add($"📌 {(_isKorean ? "유력 원인" : "Primary suspect")}: {top.ModuleName} ({conf})");
    }

    // Crash type
    if (!string.IsNullOrWhiteSpace(summary.CrashBucketKey))
    {
        lines.Add($"🔍 {(_isKorean ? "유형" : "Type")}: {summary.CrashBucketKey}");
    }

    // Module+Offset
    if (!string.IsNullOrWhiteSpace(summary.ModulePlusOffset))
    {
        lines.Add($"📍 Module+Offset: {summary.ModulePlusOffset}");
    }

    // Conclusion
    if (!string.IsNullOrWhiteSpace(summary.SummarySentence))
    {
        lines.Add($"💡 {(_isKorean ? "결론" : "Conclusion")}: {summary.SummarySentence}");
    }

    // Top recommendation
    if (summary.Recommendations.Count > 0)
    {
        lines.Add($"🛠️ {(_isKorean ? "권장" : "Action")}: {summary.Recommendations[0]}");
    }

    lines.Add($"— Tullius CTD Logger");

    return string.Join(Environment.NewLine, lines);
}
```

**Step 5: `CopyShareButton_Click` 핸들러 추가**

`CopySummaryButton_Click` 뒤에 추가:

```csharp
private void CopyShareButton_Click(object sender, RoutedEventArgs e)
{
    var text = BuildCommunityShareText();
    if (string.IsNullOrWhiteSpace(text))
    {
        StatusText.Text = T("No summary to share yet.", "아직 공유할 요약이 없습니다.");
        return;
    }

    try
    {
        var dataPackage = new DataPackage();
        dataPackage.SetText(text);
        Clipboard.SetContent(dataPackage);
        Clipboard.Flush();
        StatusText.Text = T("Copied community share summary to clipboard.", "커뮤니티 공유용 요약을 클립보드에 복사했습니다.");
    }
    catch (Exception ex)
    {
        StatusText.Text = T("Failed to copy to clipboard: ", "클립보드 복사 실패: ") + ex.Message;
    }
}
```

**Step 6: 버튼 초기 상태 및 분석 완료 시 활성화**

`MainWindow.xaml.cs` 초기화(라인 65 근처) — `CopySummaryButton.IsEnabled = false;` 옆에 추가:
```csharp
CopyShareButton.IsEnabled = false;
```

분석 완료 후(라인 373 근처) — `CopySummaryButton.IsEnabled = true;` 옆에 추가:
```csharp
CopyShareButton.IsEnabled = true;
```

**Step 7: 로컬라이제이션 — `ApplyLocalizedStaticText()` 에서 버튼 텍스트 설정**

기존 `CopySummaryButton.Content` 설정 근처에 추가:
```csharp
CopyShareButton.Content = T("📋 Share", "📋 공유");
```

**Step 8: 빌드 및 테스트**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS (XAML 가드 테스트 포함)

참고: Windows 빌드(`scripts\build-winui.cmd`)는 별도 수동 검증.

**Step 9: 커밋**

```bash
git add dump_tool_winui/MainWindow.xaml dump_tool_winui/MainWindow.xaml.cs tests/winui_xaml_tests.cpp
git commit -m "feat: add community share copy button with emoji+markdown format for Discord/Reddit"
```

---

## 최종 검증

**Step 1: 전체 Linux 테스트**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL PASS (40+ tests)

**Step 2: Windows 빌드** (가능한 경우)

Run: `scripts\build-win.cmd && scripts\build-winui.cmd`
Expected: 성공

**Step 3: 패키징** (가능한 경우)

Run: `python scripts/package.py --build-dir build-win --winui-dir build-winui --out dist/Tullius_ctd_loger.zip --no-pdb`
Expected: 성공
