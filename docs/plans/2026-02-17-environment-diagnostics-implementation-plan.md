# Environment Diagnostics Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 보고서 기반으로 Tullius CTD Logger에 환경 진단 기능 추가 — ENB/ReShade/주입 DLL 자동 진단(Phase 4A) + ESL/BEES/Missing Masters 자동 검사(Phase 4B).

**Architecture:** 기존 `plugin → helper → dump_tool` 파이프라인 유지. Phase 4A는 dump_tool에 JSON 규칙 엔진(`GraphicsInjectionDiag`)을 추가하여 모듈 목록 기반 진단. Phase 4B는 helper에 `PluginScanner`를 추가하여 캡처 시 TES4 헤더/plugins.txt를 수집, 미니덤프 user stream으로 저장, dump_tool이 읽어 규칙 적용.

**Tech Stack:** C++20, nlohmann/json (이미 의존성), Win32/DbgHelp, CTest, std::filesystem.

---

## Phase 4A: ENB/ReShade/주입 DLL 진단

### Task 1: graphics_injection_rules.json 생성 + 정적 가드 테스트

**Files:**
- Create: `dump_tool/data/graphics_injection_rules.json`
- Create: `tests/graphics_injection_rules_tests.cpp`
- Modify: `tests/CMakeLists.txt:412` (마지막 줄 이후)
- Modify: `dump_tool/CMakeLists.txt:100` (데이터 파일 목록에 추가)

**Step 1: Write the failing test**

Create `tests/graphics_injection_rules_tests.cpp`:
```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root && "SKYDIAG_PROJECT_ROOT must be set");
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestJsonFileExists()
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "graphics_injection_rules.json";
    assert(std::filesystem::exists(p) && "graphics_injection_rules.json must exist");
}

static void TestHasRequiredStructure()
{
    auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
    assert(content.find("\"version\"") != std::string::npos);
    assert(content.find("\"detection_modules\"") != std::string::npos);
    assert(content.find("\"rules\"") != std::string::npos);
}

static void TestHasEnbAndReshadeDetection()
{
    auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
    assert(content.find("\"enb\"") != std::string::npos);
    assert(content.find("\"reshade\"") != std::string::npos);
    assert(content.find("d3d11.dll") != std::string::npos);
    assert(content.find("d3dcompiler_46e.dll") != std::string::npos);
}

static void TestRulesHaveBilingualFields()
{
    auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
    assert(content.find("\"cause_ko\"") != std::string::npos);
    assert(content.find("\"cause_en\"") != std::string::npos);
    assert(content.find("\"recommendations_ko\"") != std::string::npos);
    assert(content.find("\"recommendations_en\"") != std::string::npos);
}

static void TestRulesRequireFaultModule()
{
    auto content = ReadFile("dump_tool/data/graphics_injection_rules.json");
    // All rules must have fault_module_any (crash location required, not just module presence)
    assert(content.find("\"fault_module_any\"") != std::string::npos);
}

int main()
{
    TestJsonFileExists();
    TestHasRequiredStructure();
    TestHasEnbAndReshadeDetection();
    TestRulesHaveBilingualFields();
    TestRulesRequireFaultModule();
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run:
```bash
cd /home/kdw73/Tullius_ctd_loger && cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
cmake --build build-linux-test --target skydiag_graphics_injection_rules_tests 2>&1 || echo "EXPECTED FAIL"
```
Expected: FAIL — test not registered yet, JSON doesn't exist.

**Step 3: Register test in CMakeLists.txt**

Append to `tests/CMakeLists.txt` after line 412:
```cmake

add_executable(skydiag_graphics_injection_rules_tests
  graphics_injection_rules_tests.cpp
)

set_target_properties(skydiag_graphics_injection_rules_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_rules_tests COMMAND skydiag_graphics_injection_rules_tests)
set_tests_properties(skydiag_graphics_injection_rules_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

**Step 4: Create graphics_injection_rules.json**

Create `dump_tool/data/graphics_injection_rules.json`:
```json
{
  "version": 1,
  "detection_modules": {
    "enb": ["d3d11.dll", "d3dcompiler_46e.dll"],
    "reshade": ["reshade.dll", "reshade64.dll", "dxgi.dll"],
    "dxvk": ["d3d9.dll", "dxvk_d3d11.dll"]
  },
  "rules": [
    {
      "id": "CRASH_IN_ENB",
      "detect": {
        "modules_any": ["d3d11.dll", "d3dcompiler_46e.dll"],
        "fault_module_any": ["d3d11.dll", "d3dcompiler_46e.dll"]
      },
      "diagnosis": {
        "cause_ko": "ENB/렌더링 인젝션 DLL 내부에서 크래시 발생",
        "cause_en": "Crash occurred inside ENB/rendering injection DLL",
        "confidence": "high",
        "recommendations_ko": [
          "[ENB] enblocal.ini 설정(VideoMemorySizeMb, EnableOcclusionCulling 등) 점검",
          "[ENB] 최신 ENB 바이너리로 업데이트 시도",
          "[ENB] ENB를 일시 제거하고 크래시 재현 여부 확인"
        ],
        "recommendations_en": [
          "[ENB] Check enblocal.ini settings (VideoMemorySizeMb, EnableOcclusionCulling, etc.)",
          "[ENB] Try updating to the latest ENB binaries",
          "[ENB] Temporarily remove ENB and check if crash reproduces"
        ]
      }
    },
    {
      "id": "CRASH_IN_RESHADE",
      "detect": {
        "modules_any": ["reshade.dll", "reshade64.dll", "dxgi.dll"],
        "fault_module_any": ["reshade.dll", "reshade64.dll", "dxgi.dll"]
      },
      "diagnosis": {
        "cause_ko": "ReShade/렌더링 인젝션 DLL 내부에서 크래시 발생",
        "cause_en": "Crash occurred inside ReShade/rendering injection DLL",
        "confidence": "high",
        "recommendations_ko": [
          "[ReShade] ReShade 버전 업데이트 또는 프리셋 변경 시도",
          "[ReShade] ReShade를 일시 제거하고 크래시 재현 여부 확인"
        ],
        "recommendations_en": [
          "[ReShade] Try updating ReShade or changing presets",
          "[ReShade] Temporarily remove ReShade and check if crash reproduces"
        ]
      }
    },
    {
      "id": "CRASH_IN_INPUT_INJECTION",
      "detect": {
        "modules_any": ["dinput8.dll"],
        "fault_module_any": ["dinput8.dll"]
      },
      "diagnosis": {
        "cause_ko": "입력 인젝션 DLL(dinput8.dll) 내부에서 크래시 발생",
        "cause_en": "Crash occurred inside input injection DLL (dinput8.dll)",
        "confidence": "medium",
        "recommendations_ko": [
          "[입력] dinput8.dll을 사용하는 모드(컨트롤러 모드 등) 점검",
          "[입력] dinput8.dll을 일시 제거하고 크래시 재현 여부 확인"
        ],
        "recommendations_en": [
          "[Input] Check mods using dinput8.dll (controller mods, etc.)",
          "[Input] Temporarily remove dinput8.dll and check if crash reproduces"
        ]
      }
    }
  ]
}
```

**Step 5: Add to data file list in dump_tool/CMakeLists.txt**

Modify `dump_tool/CMakeLists.txt:97-101` — add `"graphics_injection_rules.json"` to the data file list:

Replace:
```cmake
set(SKYDIAG_DUMP_TOOL_DATA_FILES
  "hook_frameworks.json"
  "crash_signatures.json"
  "address_db/skyrimse_functions.json"
)
```

With:
```cmake
set(SKYDIAG_DUMP_TOOL_DATA_FILES
  "hook_frameworks.json"
  "crash_signatures.json"
  "graphics_injection_rules.json"
  "address_db/skyrimse_functions.json"
)
```

**Step 6: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test --target skydiag_graphics_injection_rules_tests && ctest --test-dir build-linux-test -R graphics_injection_rules --output-on-failure
```
Expected: PASS — all 5 assertions pass.

**Step 7: Commit**

```bash
git add dump_tool/data/graphics_injection_rules.json tests/graphics_injection_rules_tests.cpp tests/CMakeLists.txt dump_tool/CMakeLists.txt
git commit -m "feat: add graphics injection rules JSON and static guard tests"
```

---

### Task 2: GraphicsInjectionDiag engine

**Files:**
- Create: `dump_tool/src/GraphicsInjectionDiag.h`
- Create: `dump_tool/src/GraphicsInjectionDiag.cpp`
- Modify: `dump_tool/CMakeLists.txt:7-48` (소스 목록에 추가)
- Create: `tests/graphics_injection_diag_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/graphics_injection_diag_tests.cpp`:
```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestHeaderApiExists()
{
    auto header = ReadFile("dump_tool/src/GraphicsInjectionDiag.h");
    assert(header.find("GraphicsInjectionDiag") != std::string::npos);
    assert(header.find("GraphicsEnvironment") != std::string::npos);
    assert(header.find("GraphicsDiagResult") != std::string::npos);
    assert(header.find("LoadRules") != std::string::npos);
    assert(header.find("DetectEnvironment") != std::string::npos);
    assert(header.find("Diagnose") != std::string::npos);
}

static void TestImplExists()
{
    auto impl = ReadFile("dump_tool/src/GraphicsInjectionDiag.cpp");
    assert(impl.find("LoadRules") != std::string::npos);
    assert(impl.find("DetectEnvironment") != std::string::npos);
    assert(impl.find("Diagnose") != std::string::npos);
    assert(impl.find("nlohmann") != std::string::npos);
}

static void TestImplUsesLowerCaseComparison()
{
    auto impl = ReadFile("dump_tool/src/GraphicsInjectionDiag.cpp");
    // Must compare module names case-insensitively
    assert((impl.find("WideLower") != std::string::npos ||
            impl.find("towlower") != std::string::npos ||
            impl.find("LowerCopy") != std::string::npos)
        && "Must use case-insensitive module comparison");
}

int main()
{
    TestHeaderApiExists();
    TestImplExists();
    TestImplUsesLowerCaseComparison();
    return 0;
}
```

**Step 2: Register test, run to verify RED**

Append to `tests/CMakeLists.txt`:
```cmake

add_executable(skydiag_graphics_injection_diag_tests
  graphics_injection_diag_tests.cpp
)

set_target_properties(skydiag_graphics_injection_diag_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_diag_tests COMMAND skydiag_graphics_injection_diag_tests)
set_tests_properties(skydiag_graphics_injection_diag_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

Run:
```bash
cmake --build build-linux-test --target skydiag_graphics_injection_diag_tests 2>&1 || echo "EXPECTED FAIL"
```
Expected: FAIL — source files don't exist.

**Step 3: Create GraphicsInjectionDiag.h**

Create `dump_tool/src/GraphicsInjectionDiag.h`:
```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "I18nCore.h"

namespace skydiag::dump_tool {

struct GraphicsEnvironment
{
    bool enb_detected = false;
    bool reshade_detected = false;
    bool dxvk_detected = false;
    std::vector<std::wstring> injection_modules;  // detected injection DLLs
};

struct GraphicsDiagResult
{
    std::string rule_id;
    i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
    std::wstring confidence;
    std::wstring cause;
    std::vector<std::wstring> recommendations;
};

class GraphicsInjectionDiag
{
public:
    bool LoadRules(const std::filesystem::path& jsonPath);

    GraphicsEnvironment DetectEnvironment(
        const std::vector<std::wstring>& moduleFilenames) const;

    std::optional<GraphicsDiagResult> Diagnose(
        const std::vector<std::wstring>& moduleFilenames,
        const std::wstring& faultModuleFilename,
        bool useKorean) const;

    std::size_t RuleCount() const { return m_rules.size(); }

private:
    struct DetectionGroup { std::wstring name; std::vector<std::wstring> dlls; };
    struct Rule;

    std::vector<DetectionGroup> m_groups;
    std::vector<Rule> m_rules;
};

}  // namespace skydiag::dump_tool
```

**Step 4: Create GraphicsInjectionDiag.cpp**

Create `dump_tool/src/GraphicsInjectionDiag.cpp`:
```cpp
#include "GraphicsInjectionDiag.h"

#include <algorithm>
#include <cwctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

namespace {

std::wstring WideLower(std::wstring_view sv)
{
    std::wstring s(sv);
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(static_cast<unsigned int>(c)));
    return s;
}

std::wstring Utf8ToWide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

bool ContainsModule(const std::vector<std::wstring>& moduleFilenames,
                    const std::vector<std::wstring>& targets)
{
    for (const auto& t : targets) {
        for (const auto& m : moduleFilenames) {
            if (WideLower(m) == t) return true;
        }
    }
    return false;
}

bool ContainsAllModules(const std::vector<std::wstring>& moduleFilenames,
                        const std::vector<std::wstring>& targets)
{
    for (const auto& t : targets) {
        bool found = false;
        for (const auto& m : moduleFilenames) {
            if (WideLower(m) == t) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

}  // namespace

struct GraphicsInjectionDiag::Rule
{
    std::string id;
    std::vector<std::wstring> modules_any;       // lowercase
    std::vector<std::wstring> modules_all;       // lowercase
    std::vector<std::wstring> fault_module_any;  // lowercase
    std::wstring cause_ko;
    std::wstring cause_en;
    std::string confidence;
    std::vector<std::wstring> recommendations_ko;
    std::vector<std::wstring> recommendations_en;
};

bool GraphicsInjectionDiag::LoadRules(const std::filesystem::path& jsonPath)
{
    try {
        std::ifstream f(jsonPath);
        if (!f.is_open()) return false;
        auto j = nlohmann::json::parse(f);

        m_groups.clear();
        if (j.contains("detection_modules")) {
            for (const auto& [name, dlls] : j["detection_modules"].items()) {
                DetectionGroup g;
                g.name = Utf8ToWide(name);
                for (const auto& d : dlls) {
                    g.dlls.push_back(WideLower(Utf8ToWide(d.get<std::string>())));
                }
                m_groups.push_back(std::move(g));
            }
        }

        m_rules.clear();
        for (const auto& r : j.at("rules")) {
            Rule rule;
            rule.id = r.at("id").get<std::string>();

            const auto& det = r.at("detect");
            if (det.contains("modules_any")) {
                for (const auto& m : det["modules_any"]) {
                    rule.modules_any.push_back(WideLower(Utf8ToWide(m.get<std::string>())));
                }
            }
            if (det.contains("modules_all")) {
                for (const auto& m : det["modules_all"]) {
                    rule.modules_all.push_back(WideLower(Utf8ToWide(m.get<std::string>())));
                }
            }
            if (det.contains("fault_module_any")) {
                for (const auto& m : det["fault_module_any"]) {
                    rule.fault_module_any.push_back(WideLower(Utf8ToWide(m.get<std::string>())));
                }
            }

            const auto& diag = r.at("diagnosis");
            rule.cause_ko = Utf8ToWide(diag.at("cause_ko").get<std::string>());
            rule.cause_en = Utf8ToWide(diag.at("cause_en").get<std::string>());
            rule.confidence = diag.at("confidence").get<std::string>();
            for (const auto& rec : diag.at("recommendations_ko")) {
                rule.recommendations_ko.push_back(Utf8ToWide(rec.get<std::string>()));
            }
            for (const auto& rec : diag.at("recommendations_en")) {
                rule.recommendations_en.push_back(Utf8ToWide(rec.get<std::string>()));
            }
            m_rules.push_back(std::move(rule));
        }
        return true;
    } catch (...) {
        return false;
    }
}

GraphicsEnvironment GraphicsInjectionDiag::DetectEnvironment(
    const std::vector<std::wstring>& moduleFilenames) const
{
    GraphicsEnvironment env;
    for (const auto& g : m_groups) {
        if (ContainsModule(moduleFilenames, g.dlls)) {
            if (g.name == L"enb") env.enb_detected = true;
            else if (g.name == L"reshade") env.reshade_detected = true;
            else if (g.name == L"dxvk") env.dxvk_detected = true;

            for (const auto& dll : g.dlls) {
                for (const auto& m : moduleFilenames) {
                    if (WideLower(m) == dll) {
                        env.injection_modules.push_back(m);
                    }
                }
            }
        }
    }
    return env;
}

std::optional<GraphicsDiagResult> GraphicsInjectionDiag::Diagnose(
    const std::vector<std::wstring>& moduleFilenames,
    const std::wstring& faultModuleFilename,
    bool useKorean) const
{
    const std::wstring faultLower = WideLower(faultModuleFilename);

    for (const auto& rule : m_rules) {
        // Check modules_any
        if (!rule.modules_any.empty() && !ContainsModule(moduleFilenames, rule.modules_any)) continue;
        // Check modules_all
        if (!rule.modules_all.empty() && !ContainsAllModules(moduleFilenames, rule.modules_all)) continue;
        // Check fault_module_any (required)
        if (!rule.fault_module_any.empty()) {
            bool faultMatch = false;
            for (const auto& fm : rule.fault_module_any) {
                if (faultLower == fm) { faultMatch = true; break; }
            }
            if (!faultMatch) continue;
        }

        GraphicsDiagResult result;
        result.rule_id = rule.id;
        result.cause = useKorean ? rule.cause_ko : rule.cause_en;
        result.recommendations = useKorean ? rule.recommendations_ko : rule.recommendations_en;

        if (rule.confidence == "high") {
            result.confidence_level = i18n::ConfidenceLevel::kHigh;
        } else if (rule.confidence == "medium") {
            result.confidence_level = i18n::ConfidenceLevel::kMedium;
        } else {
            result.confidence_level = i18n::ConfidenceLevel::kLow;
        }
        result.confidence = useKorean
            ? (rule.confidence == "high" ? L"높음" : rule.confidence == "medium" ? L"중간" : L"낮음")
            : (rule.confidence == "high" ? L"High" : rule.confidence == "medium" ? L"Medium" : L"Low");

        return result;
    }
    return std::nullopt;
}

}  // namespace skydiag::dump_tool
```

**Step 5: Add to dump_tool/CMakeLists.txt source list**

Modify `dump_tool/CMakeLists.txt:7-48` — add after line 29 (`src/EvidenceBuilderInternalsUtil.cpp`):
```cmake
  src/GraphicsInjectionDiag.cpp
  src/GraphicsInjectionDiag.h
```

**Step 6: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test --target skydiag_graphics_injection_diag_tests && ctest --test-dir build-linux-test -R graphics_injection_diag --output-on-failure
```
Expected: PASS.

**Step 7: Commit**

```bash
git add dump_tool/src/GraphicsInjectionDiag.h dump_tool/src/GraphicsInjectionDiag.cpp dump_tool/CMakeLists.txt tests/graphics_injection_diag_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add GraphicsInjectionDiag engine for ENB/ReShade/injection DLL diagnostics"
```

---

### Task 3: Integrate GraphicsInjectionDiag into Analyzer pipeline

**Files:**
- Modify: `dump_tool/src/Analyzer.h:91-93` — add graphics fields to AnalysisResult
- Modify: `dump_tool/src/Analyzer.cpp:94-97` — call graphics diagnostics after hook framework loading
- Create: `tests/graphics_injection_integration_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/graphics_injection_integration_tests.cpp`:
```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestAnalysisResultHasGraphicsFields()
{
    auto header = ReadFile("dump_tool/src/Analyzer.h");
    assert(header.find("GraphicsEnvironment") != std::string::npos
        && "AnalysisResult must include GraphicsEnvironment");
    assert(header.find("GraphicsDiagResult") != std::string::npos
        && "AnalysisResult must include GraphicsDiagResult");
    assert(header.find("graphics_env") != std::string::npos);
    assert(header.find("graphics_diag") != std::string::npos);
}

static void TestAnalyzerCallsGraphicsDiag()
{
    auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
    assert(impl.find("GraphicsInjectionDiag") != std::string::npos
        && "Analyzer must use GraphicsInjectionDiag");
    assert(impl.find("DetectEnvironment") != std::string::npos);
    assert(impl.find("graphics_injection_rules.json") != std::string::npos);
}

static void TestEvidenceUsesGraphicsDiag()
{
    auto impl = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
    assert(impl.find("graphics_diag") != std::string::npos
        && "Evidence builder must use graphics_diag");
}

static void TestRecommendationsUseGraphicsDiag()
{
    auto impl = ReadFile("dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp");
    assert(impl.find("graphics_diag") != std::string::npos
        && "Recommendations builder must use graphics_diag");
}

static void TestOutputWriterHasGraphicsFields()
{
    auto impl = ReadFile("dump_tool/src/OutputWriter.cpp");
    assert(impl.find("\"graphics_environment\"") != std::string::npos);
}

int main()
{
    TestAnalysisResultHasGraphicsFields();
    TestAnalyzerCallsGraphicsDiag();
    TestEvidenceUsesGraphicsDiag();
    TestRecommendationsUseGraphicsDiag();
    TestOutputWriterHasGraphicsFields();
    return 0;
}
```

**Step 2: Register test, run to verify RED**

Append to `tests/CMakeLists.txt`:
```cmake

add_executable(skydiag_graphics_injection_integration_tests
  graphics_injection_integration_tests.cpp
)

set_target_properties(skydiag_graphics_injection_integration_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_graphics_injection_integration_tests COMMAND skydiag_graphics_injection_integration_tests)
set_tests_properties(skydiag_graphics_injection_integration_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

Run:
```bash
cmake --build build-linux-test --target skydiag_graphics_injection_integration_tests && ctest --test-dir build-linux-test -R graphics_injection_integration --output-on-failure
```
Expected: FAIL — fields not present yet.

**Step 3: Add fields to AnalysisResult**

Modify `dump_tool/src/Analyzer.h` — add `#include "GraphicsInjectionDiag.h"` after line 11:
```cpp
#include "GraphicsInjectionDiag.h"
```

Add after line 93 (`std::vector<ModuleStats> history_stats;`):
```cpp
  GraphicsEnvironment graphics_env;
  std::optional<GraphicsDiagResult> graphics_diag;
```

**Step 4: Call graphics diagnostics in Analyzer.cpp**

Modify `dump_tool/src/Analyzer.cpp` — add `#include "GraphicsInjectionDiag.h"` in the include block (after line 9):
```cpp
#include "GraphicsInjectionDiag.h"
```

Add after line 97 (after `LoadHookFrameworksFromJson` block, before `LoadAllModules`):
```cpp

  // Graphics injection environment detection and diagnostics.
  GraphicsInjectionDiag graphicsDiag;
  const bool graphicsRulesLoaded = !opt.data_dir.empty() &&
    graphicsDiag.LoadRules(std::filesystem::path(opt.data_dir) / L"graphics_injection_rules.json");
```

Then after line 109 (after the `game_version` detection loop ends), add:
```cpp

  // Detect graphics injection environment from loaded modules.
  if (graphicsRulesLoaded) {
    std::vector<std::wstring> moduleFilenames;
    moduleFilenames.reserve(allModules.size());
    for (const auto& m : allModules) {
      moduleFilenames.push_back(m.filename);
    }
    out.graphics_env = graphicsDiag.DetectEnvironment(moduleFilenames);
    out.graphics_diag = graphicsDiag.Diagnose(moduleFilenames, out.fault_module_filename,
      opt.language == i18n::Language::kKorean);
  }
```

**Step 5: Add graphics evidence**

Modify `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp` — add after the signature_match evidence block (after line 39, before `if (hasException)`):

```cpp

  if (r.graphics_diag.has_value()) {
    const auto& gd = *r.graphics_diag;
    EvidenceItem e{};
    e.confidence_level = gd.confidence_level;
    e.confidence = gd.confidence.empty() ? ConfidenceText(lang, gd.confidence_level) : gd.confidence;
    e.title = ctx.en
      ? (L"Graphics injection crash: " + std::wstring(gd.rule_id.begin(), gd.rule_id.end()))
      : (L"그래픽 인젝션 크래시: " + std::wstring(gd.rule_id.begin(), gd.rule_id.end()));
    e.details = gd.cause;
    r.evidence.push_back(std::move(e));
  }
```

**Step 6: Add graphics recommendations**

Modify `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp` — add after the signature_match recommendations block (after line 54, the closing `}` of the `if (r.signature_match.has_value())` block):

```cpp

  if (r.graphics_diag.has_value()) {
    for (const auto& rec : r.graphics_diag->recommendations) {
      if (!rec.empty()) {
        r.recommendations.push_back(rec);
      }
    }
  }
```

**Step 7: Add graphics JSON output**

Modify `dump_tool/src/OutputWriter.cpp` — add after line 154 (after the `resolved_functions` block, before `summary["suspects"]`):

```cpp

  summary["graphics_environment"] = {
    { "enb_detected", r.graphics_env.enb_detected },
    { "reshade_detected", r.graphics_env.reshade_detected },
    { "dxvk_detected", r.graphics_env.dxvk_detected },
  };
  if (r.graphics_diag.has_value()) {
    summary["graphics_diagnosis"] = {
      { "rule_id", r.graphics_diag->rule_id },
      { "cause", WideToUtf8(r.graphics_diag->cause) },
      { "confidence", WideToUtf8(r.graphics_diag->confidence) },
    };
  } else {
    summary["graphics_diagnosis"] = nullptr;
  }
```

**Step 8: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test && ctest --test-dir build-linux-test -R graphics_injection --output-on-failure
```
Expected: ALL PASS (3 graphics tests).

**Step 9: Commit**

```bash
git add dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/EvidenceBuilderInternalsEvidence.cpp dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp dump_tool/src/OutputWriter.cpp tests/graphics_injection_integration_tests.cpp tests/CMakeLists.txt
git commit -m "feat: integrate graphics injection diagnostics into analysis pipeline"
```

---

## Phase 4B: ESL/BEES/Missing Masters 진단

### Task 4: Protocol constant for PluginInfo user stream

**Files:**
- Modify: `shared/SkyrimDiagProtocol.h:14` — add new stream constant

**Step 1: Add the constant**

Modify `shared/SkyrimDiagProtocol.h:14` — add after `kMinidumpUserStream_WctJson`:

```cpp
inline constexpr std::uint32_t kMinidumpUserStream_PluginInfo = 0x10000u + 0x504Cu;  // arbitrary "PL"
```

**Step 2: Commit**

```bash
git add shared/SkyrimDiagProtocol.h
git commit -m "feat: add kMinidumpUserStream_PluginInfo protocol constant"
```

---

### Task 5: PluginScanner — TES4 header parser + plugins.txt reader

**Files:**
- Create: `helper/src/PluginScanner.h`
- Create: `helper/src/PluginScanner.cpp`
- Create: `helper/include/SkyrimDiagHelper/PluginScanner.h` (public header)
- Modify: `helper/CMakeLists.txt:7-32` — add source file
- Create: `tests/plugin_scanner_tests.cpp`
- Create: `tests/data/test_plugin_esl.bin` (test data — 42-byte minimal TES4 record)
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/plugin_scanner_tests.cpp`:
```cpp
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestPluginScannerHeaderExists()
{
    auto header = ReadFile("helper/src/PluginScanner.h");
    assert(header.find("PluginMeta") != std::string::npos);
    assert(header.find("PluginScanResult") != std::string::npos);
    assert(header.find("ScanPlugins") != std::string::npos);
    assert(header.find("ParseTes4Header") != std::string::npos);
    assert(header.find("ParsePluginsTxt") != std::string::npos);
}

static void TestPluginScannerImplExists()
{
    auto impl = ReadFile("helper/src/PluginScanner.cpp");
    assert(impl.find("ParseTes4Header") != std::string::npos);
    assert(impl.find("TES4") != std::string::npos);
    assert(impl.find("MAST") != std::string::npos);
    assert(impl.find("0x0200") != std::string::npos && "Must check ESL flag 0x0200");
}

static void TestTestDataExists()
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / "tests" / "data" / "test_plugin_esl.bin";
    assert(std::filesystem::exists(p) && "Test ESP binary must exist");
    auto size = std::filesystem::file_size(p);
    assert(size >= 24 && "Minimal TES4 header is at least 24 bytes");
}

int main()
{
    TestPluginScannerHeaderExists();
    TestPluginScannerImplExists();
    TestTestDataExists();
    return 0;
}
```

**Step 2: Register test, run to verify RED**

Append to `tests/CMakeLists.txt`:
```cmake

add_executable(skydiag_plugin_scanner_tests
  plugin_scanner_tests.cpp
)

set_target_properties(skydiag_plugin_scanner_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_plugin_scanner_tests COMMAND skydiag_plugin_scanner_tests)
set_tests_properties(skydiag_plugin_scanner_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

Expected: FAIL.

**Step 3: Create test data — minimal TES4 record with ESL flag**

Create `tests/data/test_plugin_esl.bin` — this is a binary file. Use a script to generate it:

```bash
python3 -c "
import struct
# TES4 record: type(4) + data_size(4) + flags(4) + formid(4) + vc(4) + header_version(4) = 24 bytes header
# Then HEDR subrecord: type(4) + size(2) + data(12) = 18 bytes
data = b'TES4'                          # record type
data += struct.pack('<I', 18)            # data size (HEDR subrecord)
data += struct.pack('<I', 0x0200)        # flags: ESL set
data += struct.pack('<I', 0)             # formid
data += struct.pack('<I', 0)             # vc info
data += struct.pack('<f', 1.71)          # header version
data += b'HEDR'                          # subrecord type
data += struct.pack('<H', 12)            # subrecord size
data += struct.pack('<f', 1.71)          # hedr version
data += struct.pack('<I', 0)             # num records
data += struct.pack('<I', 0)             # next object id
with open('tests/data/test_plugin_esl.bin', 'wb') as f:
    f.write(data)
print(f'Written {len(data)} bytes')
"
```

**Step 4: Create PluginScanner.h**

Create `helper/src/PluginScanner.h`:
```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skydiag::helper {

struct PluginMeta
{
    std::string filename;
    float header_version = 0.0f;
    bool is_esl = false;
    bool is_active = false;
    std::vector<std::string> masters;
};

struct PluginScanResult
{
    std::string game_exe_version;
    std::string plugins_source;  // "standard", "mo2_profile", "fallback", "error"
    bool mo2_detected = false;
    std::vector<PluginMeta> plugins;
    std::string error;
};

// Parse a TES4 record header from raw bytes. Returns false if data is too small or invalid.
bool ParseTes4Header(const std::uint8_t* data, std::size_t size, PluginMeta& out);

// Parse plugins.txt content into a list of active plugin filenames.
std::vector<std::string> ParsePluginsTxt(const std::string& content);

// Full plugin scan: find plugins.txt, parse headers, detect MO2.
PluginScanResult ScanPlugins(
    const std::filesystem::path& gameExeDir,
    const std::vector<std::wstring>& moduleFilenames);

// Serialize PluginScanResult to JSON (UTF-8).
std::string SerializePluginScanResult(const PluginScanResult& result);

}  // namespace skydiag::helper
```

**Step 5: Create PluginScanner.cpp**

Create `helper/src/PluginScanner.cpp`:
```cpp
#include "PluginScanner.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <ShlObj.h>
#endif

namespace skydiag::helper {

namespace {

std::wstring WideLower(std::wstring_view sv)
{
    std::wstring s(sv);
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(static_cast<unsigned int>(c)));
    return s;
}

bool HasModule(const std::vector<std::wstring>& modules, const wchar_t* name)
{
    const std::wstring lower(name);
    for (const auto& m : modules) {
        if (WideLower(m) == lower) return true;
    }
    return false;
}

}  // namespace

bool ParseTes4Header(const std::uint8_t* data, std::size_t size, PluginMeta& out)
{
    // Minimum: 4 (type) + 4 (data_size) + 4 (flags) + 4 (formid) + 4 (vc) + 4 (header_version) = 24
    if (!data || size < 24) return false;
    if (std::memcmp(data, "TES4", 4) != 0) return false;

    std::uint32_t dataSize = 0;
    std::memcpy(&dataSize, data + 4, 4);

    std::uint32_t flags = 0;
    std::memcpy(&flags, data + 8, 4);
    out.is_esl = (flags & 0x0200u) != 0;

    float headerVer = 0.0f;
    std::memcpy(&headerVer, data + 20, 4);
    out.header_version = headerVer;

    // Parse sub-records for MAST entries
    const std::size_t headerEnd = 24;
    const std::size_t recordEnd = std::min<std::size_t>(size, headerEnd + dataSize);
    std::size_t pos = headerEnd;
    while (pos + 6 <= recordEnd) {
        char subType[5]{};
        std::memcpy(subType, data + pos, 4);
        std::uint16_t subSize = 0;
        std::memcpy(&subSize, data + pos + 4, 2);
        pos += 6;

        if (std::strcmp(subType, "MAST") == 0 && subSize > 0 && pos + subSize <= recordEnd) {
            std::string master(reinterpret_cast<const char*>(data + pos), subSize);
            // Remove null terminator if present
            if (!master.empty() && master.back() == '\0') master.pop_back();
            if (!master.empty()) {
                out.masters.push_back(std::move(master));
            }
        }

        pos += subSize;
    }
    return true;
}

std::vector<std::string> ParsePluginsTxt(const std::string& content)
{
    std::vector<std::string> active;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '*') {
            active.push_back(line.substr(1));
        }
        // Lines without '*' prefix are inactive — skip
    }
    return active;
}

PluginScanResult ScanPlugins(
    const std::filesystem::path& gameExeDir,
    const std::vector<std::wstring>& moduleFilenames)
{
    PluginScanResult result;
    result.mo2_detected = HasModule(moduleFilenames, L"usvfs_x64.dll");

    // 1. Find plugins.txt
    std::filesystem::path pluginsTxtPath;

    if (result.mo2_detected) {
        // Try portable MO2: look for ModOrganizer.ini near game exe
        auto moIni = gameExeDir / "ModOrganizer.ini";
        if (!std::filesystem::exists(moIni)) {
            moIni = gameExeDir.parent_path() / "ModOrganizer.ini";
        }
        if (std::filesystem::exists(moIni)) {
            // Parse selected_profile from ini
            std::ifstream ini(moIni);
            std::string iniLine;
            std::string selectedProfile;
            while (std::getline(ini, iniLine)) {
                if (iniLine.find("selected_profile=") == 0) {
                    selectedProfile = iniLine.substr(17);
                    // Trim
                    while (!selectedProfile.empty() && (selectedProfile.back() == '\r' || selectedProfile.back() == '\n')) {
                        selectedProfile.pop_back();
                    }
                    break;
                }
            }
            if (!selectedProfile.empty()) {
                auto profileTxt = moIni.parent_path() / "profiles" / selectedProfile / "plugins.txt";
                if (std::filesystem::exists(profileTxt)) {
                    pluginsTxtPath = profileTxt;
                    result.plugins_source = "mo2_profile";
                }
            }
        }
    }

    // Fallback: standard location
    if (pluginsTxtPath.empty()) {
#ifdef _WIN32
        wchar_t* localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
            pluginsTxtPath = std::filesystem::path(localAppData) / L"Skyrim Special Edition" / L"plugins.txt";
            CoTaskMemFree(localAppData);
        }
#endif
        if (!pluginsTxtPath.empty() && std::filesystem::exists(pluginsTxtPath)) {
            if (result.plugins_source.empty()) result.plugins_source = "standard";
        } else {
            result.plugins_source = "error";
            result.error = "Could not find plugins.txt";
            return result;
        }
    }

    // 2. Parse plugins.txt
    std::ifstream ptf(pluginsTxtPath);
    if (!ptf.is_open()) {
        result.plugins_source = "error";
        result.error = "Could not open plugins.txt";
        return result;
    }
    std::string ptContent((std::istreambuf_iterator<char>(ptf)), std::istreambuf_iterator<char>());
    auto activePlugins = ParsePluginsTxt(ptContent);

    // 3. Parse TES4 headers for each active plugin
    const auto dataDir = gameExeDir / "Data";
    // For MO2, we might also need to search mods/ folders — but Data may have overwrite
    for (const auto& pluginName : activePlugins) {
        PluginMeta meta;
        meta.filename = pluginName;
        meta.is_active = true;

        // Try to find the .esp/.esm/.esl file
        auto pluginPath = dataDir / pluginName;
        if (std::filesystem::exists(pluginPath)) {
            std::ifstream pf(pluginPath, std::ios::binary);
            if (pf.is_open()) {
                // Read first 4KB (more than enough for TES4 + masters)
                std::vector<std::uint8_t> buf(4096);
                pf.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
                const auto bytesRead = static_cast<std::size_t>(pf.gcount());
                ParseTes4Header(buf.data(), bytesRead, meta);
            }
        }
        // If file not found (MO2 VFS), we still record the plugin name
        result.plugins.push_back(std::move(meta));
    }

    return result;
}

std::string SerializePluginScanResult(const PluginScanResult& result)
{
    nlohmann::json j;
    j["game_exe_version"] = result.game_exe_version;
    j["plugins_source"] = result.plugins_source;
    j["mo2_detected"] = result.mo2_detected;
    j["error"] = result.error;
    j["plugins"] = nlohmann::json::array();

    for (const auto& p : result.plugins) {
        nlohmann::json pj;
        pj["filename"] = p.filename;
        pj["header_version"] = p.header_version;
        pj["is_esl"] = p.is_esl;
        pj["is_active"] = p.is_active;
        pj["masters"] = p.masters;
        j["plugins"].push_back(std::move(pj));
    }

    return j.dump();
}

}  // namespace skydiag::helper
```

**Step 6: Add to helper/CMakeLists.txt**

Modify `helper/CMakeLists.txt:7-32` — add after line 12 (`src/DumpWriter.cpp`):
```cmake
  src/PluginScanner.cpp
  src/PluginScanner.h
```

**Step 7: Generate test binary data**

Run:
```bash
mkdir -p tests/data && python3 -c "
import struct
data = b'TES4'
data += struct.pack('<I', 18)
data += struct.pack('<I', 0x0200)
data += struct.pack('<I', 0)
data += struct.pack('<I', 0)
data += struct.pack('<f', 1.71)
data += b'HEDR'
data += struct.pack('<H', 12)
data += struct.pack('<f', 1.71)
data += struct.pack('<I', 0)
data += struct.pack('<I', 0)
with open('tests/data/test_plugin_esl.bin', 'wb') as f:
    f.write(data)
print(f'Written {len(data)} bytes')
"
```

**Step 8: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test --target skydiag_plugin_scanner_tests && ctest --test-dir build-linux-test -R plugin_scanner --output-on-failure
```
Expected: PASS.

**Step 9: Commit**

```bash
git add helper/src/PluginScanner.h helper/src/PluginScanner.cpp helper/CMakeLists.txt tests/plugin_scanner_tests.cpp tests/data/test_plugin_esl.bin tests/CMakeLists.txt
git commit -m "feat: add PluginScanner for TES4 header parsing and plugins.txt reading"
```

---

### Task 6: Integrate PluginScanner into DumpWriter

**Files:**
- Modify: `helper/src/DumpWriter.cpp:33-86` — add PluginInfo user stream
- Modify: `helper/include/SkyrimDiagHelper/DumpWriter.h` — update function signature
- Create: `tests/plugin_stream_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/plugin_stream_tests.cpp`:
```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestDumpWriterIncludesPluginStream()
{
    auto impl = ReadFile("helper/src/DumpWriter.cpp");
    assert(impl.find("kMinidumpUserStream_PluginInfo") != std::string::npos
        && "DumpWriter must include PluginInfo user stream");
}

static void TestDumpWriterReservesThreeStreams()
{
    auto impl = ReadFile("helper/src/DumpWriter.cpp");
    assert(impl.find("reserve(3)") != std::string::npos
        && "DumpWriter must reserve space for 3 user streams");
}

int main()
{
    TestDumpWriterIncludesPluginStream();
    TestDumpWriterReservesThreeStreams();
    return 0;
}
```

**Step 2: Register test, verify RED**

Append to `tests/CMakeLists.txt`:
```cmake

add_executable(skydiag_plugin_stream_tests
  plugin_stream_tests.cpp
)

set_target_properties(skydiag_plugin_stream_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_plugin_stream_tests COMMAND skydiag_plugin_stream_tests)
set_tests_properties(skydiag_plugin_stream_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

Expected: FAIL.

**Step 3: Modify DumpWriter.cpp**

Modify `helper/src/DumpWriter.cpp` — add include after line 11:
```cpp
#include "PluginScanner.h"
```

Change line 72 (`streams.reserve(2)`) to:
```cpp
  streams.reserve(3);
```

Add after line 86 (after the WCT stream block `}`):
```cpp

  MINIDUMP_USER_STREAM s3{};
  std::string pluginInfoJson;
  if (!pluginScanJson.empty()) {
    pluginInfoJson = pluginScanJson;
    s3.Type = skydiag::protocol::kMinidumpUserStream_PluginInfo;
    s3.BufferSize = static_cast<ULONG>(pluginInfoJson.size());
    s3.Buffer = const_cast<char*>(pluginInfoJson.data());
    streams.push_back(s3);
  }
```

Update the function signature in `helper/src/DumpWriter.cpp:33-42` — add `pluginScanJson` parameter:
```cpp
bool WriteDumpWithStreams(
  HANDLE process,
  std::uint32_t pid,
  const std::wstring& dumpPath,
  const skydiag::SharedLayout* shmSnapshot,
  std::size_t shmSnapshotBytes,
  const std::string& wctJsonUtf8,
  const std::string& pluginScanJson,
  bool isCrash,
  DumpMode dumpMode,
  std::wstring* err)
```

Update the corresponding header `helper/include/SkyrimDiagHelper/DumpWriter.h` to match the new signature — add `const std::string& pluginScanJson` parameter.

**Step 4: Update callers of WriteDumpWithStreams**

Search for all call sites of `WriteDumpWithStreams` in `helper/src/` and add the new parameter. Typically called from `CrashCapture.cpp` and `HangCapture.cpp`. Pass an empty string `""` initially — the actual plugin scan call will be integrated in a follow-up.

**Step 5: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test --target skydiag_plugin_stream_tests && ctest --test-dir build-linux-test -R plugin_stream --output-on-failure
```
Expected: PASS.

**Step 6: Commit**

```bash
git add helper/src/DumpWriter.cpp helper/include/SkyrimDiagHelper/DumpWriter.h helper/src/CrashCapture.cpp helper/src/HangCapture.cpp helper/src/ManualCapture.cpp tests/plugin_stream_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add PluginInfo user stream to DumpWriter"
```

---

### Task 7: Call PluginScanner from capture code

**Files:**
- Modify: `helper/src/CrashCapture.cpp` — call PluginScanner before WriteDumpWithStreams
- Modify: `helper/src/HangCapture.cpp` — same
- Modify: `helper/src/ManualCapture.cpp` — same (if applicable)

**Step 1: Add plugin scan call in each capture path**

In each capture file, before the `WriteDumpWithStreams` call:
```cpp
#include "PluginScanner.h"

// Before WriteDumpWithStreams call:
std::string pluginScanJson;
{
    auto gameExeDir = std::filesystem::path(/* get game exe path from process handle */);
    // Note: the implementer needs to get the game EXE path using QueryFullProcessImageNameW
    // and extract the parent directory
    std::vector<std::wstring> moduleNames;  // from shared memory or enumerate
    auto scanResult = ScanPlugins(gameExeDir, moduleNames);
    scanResult.game_exe_version = /* game version from shared memory or module version */;
    pluginScanJson = SerializePluginScanResult(scanResult);
}
```

The exact integration depends on how each capture function obtains the process handle and game path. The implementer should:
1. Use `QueryFullProcessImageNameW(processHandle, 0, buf, &size)` to get the EXE path
2. Extract parent directory
3. Get module list from the shared memory header or use EnumProcessModules
4. Pass the serialized JSON to `WriteDumpWithStreams`

**Step 2: Commit**

```bash
git add helper/src/CrashCapture.cpp helper/src/HangCapture.cpp helper/src/ManualCapture.cpp
git commit -m "feat: call PluginScanner during crash/hang capture"
```

---

### Task 8: Read PluginInfo stream in dump_tool Analyzer + plugin_rules.json

**Files:**
- Create: `dump_tool/data/plugin_rules.json`
- Modify: `dump_tool/src/Analyzer.h:93` — add plugin scan fields
- Modify: `dump_tool/src/Analyzer.cpp` — read PluginInfo stream + apply rules
- Modify: `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp` — add plugin evidence
- Modify: `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp` — add plugin recommendations
- Modify: `dump_tool/src/OutputWriter.cpp` — add plugin JSON output
- Modify: `dump_tool/CMakeLists.txt:97-101` — add plugin_rules.json to data files
- Create: `tests/plugin_rules_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write the failing test**

Create `tests/plugin_rules_tests.cpp`:
```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::string ReadFile(const char* relPath)
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / relPath;
    std::ifstream f(p);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void TestPluginRulesJsonExists()
{
    const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
    assert(root);
    std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "plugin_rules.json";
    assert(std::filesystem::exists(p));
}

static void TestHasBEESRule()
{
    auto content = ReadFile("dump_tool/data/plugin_rules.json");
    assert(content.find("BEES") != std::string::npos || content.find("bees") != std::string::npos);
    assert(content.find("1.71") != std::string::npos);
}

static void TestHasMissingMasterRule()
{
    auto content = ReadFile("dump_tool/data/plugin_rules.json");
    assert(content.find("MISSING_MASTER") != std::string::npos);
}

static void TestAnalyzerReadsPluginStream()
{
    auto impl = ReadFile("dump_tool/src/Analyzer.cpp");
    assert(impl.find("kMinidumpUserStream_PluginInfo") != std::string::npos);
}

static void TestOutputHasPluginSection()
{
    auto impl = ReadFile("dump_tool/src/OutputWriter.cpp");
    assert(impl.find("\"plugin_scan\"") != std::string::npos);
}

int main()
{
    TestPluginRulesJsonExists();
    TestHasBEESRule();
    TestHasMissingMasterRule();
    TestAnalyzerReadsPluginStream();
    TestOutputHasPluginSection();
    return 0;
}
```

**Step 2: Register test, verify RED**

Append to `tests/CMakeLists.txt` (same pattern).

**Step 3: Create plugin_rules.json**

Create `dump_tool/data/plugin_rules.json`:
```json
{
  "version": 1,
  "rules": [
    {
      "id": "HEADER_171_WITHOUT_BEES",
      "condition": {
        "any_plugin_header_version_gte": 1.71,
        "game_version_lt": "1.6.1130",
        "module_not_loaded": "bees.dll"
      },
      "diagnosis": {
        "cause_ko": "헤더 버전 1.71 플러그인이 있으나 BEES가 설치되지 않음 — 구버전 런타임에서 크래시 가능",
        "cause_en": "Header version 1.71 plugin found but BEES not installed — may crash on older runtime",
        "confidence": "high",
        "recommendations_ko": [
          "[BEES] BEES (Backported Extended ESL Support) 모드를 설치하세요",
          "[대안] 게임을 1.6.1130 이상으로 업데이트하세요"
        ],
        "recommendations_en": [
          "[BEES] Install BEES (Backported Extended ESL Support) mod",
          "[Alternative] Update game to 1.6.1130 or later"
        ]
      }
    },
    {
      "id": "MISSING_MASTER",
      "condition": {
        "has_missing_master": true
      },
      "diagnosis": {
        "cause_ko": "플러그인의 마스터 파일이 누락됨 — 로드 시 CTD 확정",
        "cause_en": "Plugin master file is missing — guaranteed CTD on load",
        "confidence": "high",
        "recommendations_ko": [
          "[필수] 누락된 마스터 모드를 설치하거나, 해당 플러그인을 비활성화하세요",
          "[도구] LOOT 또는 Wrye Bash로 마스터 누락 확인"
        ],
        "recommendations_en": [
          "[Required] Install the missing master mod or deactivate the plugin",
          "[Tool] Use LOOT or Wrye Bash to check for missing masters"
        ]
      }
    },
    {
      "id": "ESL_SLOT_NEAR_LIMIT",
      "condition": {
        "esl_count_gte": 3800
      },
      "diagnosis": {
        "cause_ko": "ESL 플러그인 수가 슬롯 한계(4096)에 근접 — 불안정할 수 있음",
        "cause_en": "ESL plugin count approaching slot limit (4096) — may become unstable",
        "confidence": "low",
        "recommendations_ko": [
          "[최적화] 불필요한 ESL 플러그인 정리 또는 병합 고려"
        ],
        "recommendations_en": [
          "[Optimization] Consider removing or merging unnecessary ESL plugins"
        ]
      }
    }
  ]
}
```

**Step 4: Add plugin fields to AnalysisResult**

Modify `dump_tool/src/Analyzer.h` — add after `graphics_diag`:
```cpp
  // Plugin scan result (from helper's PluginInfo user stream)
  bool has_plugin_scan = false;
  std::string plugin_scan_json_utf8;  // raw JSON from stream
  std::vector<std::wstring> plugin_diagnostics;  // localized diagnostic messages
  std::vector<std::wstring> missing_masters;  // detected missing master files
  bool needs_bees = false;
```

**Step 5: Read PluginInfo stream in Analyzer.cpp**

Add after the WCT stream reading block in Analyzer.cpp (search for `kMinidumpUserStream_WctJson`):
```cpp
  // Read PluginInfo user stream (from helper's plugin scanner).
  {
    void* plugPtr = nullptr;
    ULONG plugSize = 0;
    if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_PluginInfo, &plugPtr, &plugSize) &&
        plugPtr && plugSize > 0)
    {
      out.has_plugin_scan = true;
      out.plugin_scan_json_utf8 = std::string(static_cast<const char*>(plugPtr), plugSize);
    }
  }
```

Then after the graphics diagnostics block, add plugin rule application:
```cpp
  // Apply plugin diagnostic rules.
  if (out.has_plugin_scan && !out.plugin_scan_json_utf8.empty()) {
    try {
      auto pj = nlohmann::json::parse(out.plugin_scan_json_utf8);
      const bool ko = (opt.language == i18n::Language::kKorean);
      const auto& plugins = pj["plugins"];

      // Check missing masters
      std::unordered_set<std::string> activeSet;
      for (const auto& p : plugins) {
        std::string fn = p["filename"].get<std::string>();
        std::transform(fn.begin(), fn.end(), fn.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
        activeSet.insert(fn);
      }
      for (const auto& p : plugins) {
        for (const auto& m : p["masters"]) {
          std::string master = m.get<std::string>();
          std::string masterLower = master;
          std::transform(masterLower.begin(), masterLower.end(), masterLower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
          if (activeSet.find(masterLower) == activeSet.end()) {
            out.missing_masters.push_back(Utf8ToWide(master));
          }
        }
      }

      // Check BEES requirement
      bool has171 = false;
      for (const auto& p : plugins) {
        if (p["header_version"].get<float>() >= 1.709f) { has171 = true; break; }
      }
      bool gameLt1130 = false;
      if (!out.game_version.empty()) {
        // Simple version comparison
        gameLt1130 = (out.game_version < "1.6.1130");
      }
      bool hasBees = false;
      for (const auto& m : allModules) {
        if (WideLower(m.filename) == L"bees.dll") { hasBees = true; break; }
      }
      out.needs_bees = has171 && gameLt1130 && !hasBees;
    } catch (...) {
      // Parse error — ignore
    }
  }
```

**Step 6: Add plugin evidence and recommendations**

Follow same pattern as graphics — add evidence items for missing masters and BEES requirement in `EvidenceBuilderInternalsEvidence.cpp` and recommendations in `EvidenceBuilderInternalsRecommendations.cpp`.

**Step 7: Add plugin JSON output**

Modify `dump_tool/src/OutputWriter.cpp` — add after `graphics_diagnosis`:
```cpp
  if (out.has_plugin_scan) {
    summary["plugin_scan"] = nlohmann::json::parse(out.plugin_scan_json_utf8, nullptr, false);
    if (!out.missing_masters.empty()) {
      auto mm = nlohmann::json::array();
      for (const auto& m : out.missing_masters) mm.push_back(WideToUtf8(m));
      summary["missing_masters"] = std::move(mm);
    }
    summary["needs_bees"] = out.needs_bees;
  }
```

**Step 8: Add plugin_rules.json to data files**

Modify `dump_tool/CMakeLists.txt:97-101` — add `"plugin_rules.json"`:
```cmake
set(SKYDIAG_DUMP_TOOL_DATA_FILES
  "hook_frameworks.json"
  "crash_signatures.json"
  "graphics_injection_rules.json"
  "plugin_rules.json"
  "address_db/skyrimse_functions.json"
)
```

**Step 9: Run tests to verify GREEN**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test && ctest --test-dir build-linux-test --output-on-failure
```
Expected: ALL PASS.

**Step 10: Commit**

```bash
git add dump_tool/data/plugin_rules.json dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/EvidenceBuilderInternalsEvidence.cpp dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp dump_tool/src/OutputWriter.cpp dump_tool/CMakeLists.txt tests/plugin_rules_tests.cpp tests/CMakeLists.txt
git commit -m "feat: integrate plugin scan diagnostics (ESL/BEES/Missing Masters) into analysis pipeline"
```

---

### Task 9: Final verification

**Files:** (No new files)

**Step 1: Full build + test**

Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=Debug && cmake --build build-linux-test && ctest --test-dir build-linux-test --output-on-failure
```
Expected: ALL PASS.

**Step 2: Verify all new files exist**

Run:
```bash
ls -la dump_tool/data/graphics_injection_rules.json dump_tool/data/plugin_rules.json dump_tool/src/GraphicsInjectionDiag.h dump_tool/src/GraphicsInjectionDiag.cpp helper/src/PluginScanner.h helper/src/PluginScanner.cpp tests/data/test_plugin_esl.bin
```

**Step 3: Verify new test count**

Run:
```bash
ctest --test-dir build-linux-test --show-only 2>&1 | grep -c "Test #"
```
Expected: Original count + 6 new tests (graphics_injection_rules, graphics_injection_diag, graphics_injection_integration, plugin_scanner, plugin_stream, plugin_rules).

**Step 4: Check no unrelated changes**

Run:
```bash
git diff --stat HEAD~8..HEAD
```

**Step 5: Final commit (if cleanup needed)**

```bash
git add -A && git commit -m "chore: final cleanup for environment diagnostics (Phase 4)"
```
