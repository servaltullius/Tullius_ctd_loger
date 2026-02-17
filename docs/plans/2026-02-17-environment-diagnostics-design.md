# Environment Diagnostics Design (Phase 4)

> **목표:** 보고서 기반으로 Tullius CTD Logger의 환경 진단 기능을 확장.
> ENB/ReShade/주입 DLL 자동 진단 + ESL/BEES/Missing Masters 자동 검사
> **구현:** Codex에게 위임. 기존 Phase 1~3(시그니처 DB, Address Library, 크래시 이력)은 유지하고, 본 문서는 Phase 4A/4B를 정의.

## 현재 상태

### 기존 계획 (유지)
- Phase 1: 패턴 DB + 스코어링 교정 (`docs/plans/2026-02-15-analysis-reliability-improvement-design.md`)
- Phase 2: Address Library 통합
- Phase 3: 다중 크래시 통계 분석

### 기존 환경 진단 수준
- `hook_frameworks.json`에 d3d11.dll, dxgi.dll 등이 "렌더링 인젝션"으로 등록
- 모듈 분류(`MinidumpUtil.cpp:397-410`)에서 hook framework 여부만 판단
- ENB/ReShade 구분, 버전 식별, 충돌 진단 없음
- 플러그인(ESP/ESM/ESL) 메타데이터 수집/분석 없음

---

## Phase 4A: ENB/ReShade/주입 DLL 진단 강화

### 목표
모듈 목록에서 ENB/ReShade/그래픽 주입 DLL을 정밀 식별하고, **크래시 위치와 결합**하여 진단

### 핵심 원칙
- ENB와 ReShade는 동시 사용 가능 — 단순 존재를 충돌로 판단하지 않음
- "크래시 위치가 해당 DLL 내부"일 때만 진단 규칙 발동
- 단순 존재 감지는 환경 정보로 기록 (경고 아님)

### 신규 파일

#### `dump_tool/data/graphics_injection_rules.json`

```json
{
  "version": 1,
  "detection_modules": {
    "enb": ["d3d11.dll", "d3dcompiler_46e.dll"],
    "reshade": ["reshade.dll", "ReShade64.dll", "dxgi.dll"],
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
        "modules_any": ["reshade.dll", "ReShade64.dll", "dxgi.dll"],
        "fault_module_any": ["reshade.dll", "ReShade64.dll", "dxgi.dll"]
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

#### `dump_tool/src/GraphicsInjectionDiag.h`

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

struct GraphicsEnvironment
{
    bool enb_detected = false;
    bool reshade_detected = false;
    bool dxvk_detected = false;
    std::vector<std::wstring> injection_modules;  // 감지된 주입 DLL 목록
};

struct GraphicsDiagResult
{
    std::string rule_id;
    std::wstring cause;
    std::wstring confidence;
    std::vector<std::wstring> recommendations;
};

class GraphicsInjectionDiag
{
public:
    bool LoadRules(const std::filesystem::path& jsonPath);

    // 모듈 목록에서 그래픽 주입 환경 감지 (항상 실행)
    GraphicsEnvironment DetectEnvironment(
        const std::vector<std::wstring>& moduleNames) const;

    // 크래시 위치와 결합하여 진단 (fault module이 주입 DLL인 경우만)
    std::optional<GraphicsDiagResult> Diagnose(
        const std::vector<std::wstring>& moduleNames,
        const std::wstring& faultModule,
        bool useKorean) const;

private:
    struct Rule;
    std::vector<Rule> m_rules;

    struct DetectionGroup;
    std::vector<DetectionGroup> m_detectionGroups;
};

}  // namespace skydiag::dump_tool
```

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `dump_tool/src/Analyzer.h` | `AnalysisResult`에 `GraphicsEnvironment` + `optional<GraphicsDiagResult>` 추가 |
| `dump_tool/src/Analyzer.cpp` | 규칙 로드, 환경 감지, 크래시 결합 진단 호출 |
| `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp` | 그래픽 진단 결과를 Evidence 카드에 추가 |
| `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp` | 진단 시 권장사항 추가 |
| `dump_tool/src/OutputWriter.cpp` | JSON에 `graphics_environment` + `graphics_diagnosis` 섹션 |
| `dump_tool/CMakeLists.txt` | 신규 소스 파일 등록 |

### 데이터 플로우

```
미니덤프 모듈 목록
  → GraphicsInjectionDiag::DetectEnvironment()
    → GraphicsEnvironment (ENB/ReShade/DXVK 감지 여부 + 주입 DLL 목록)
    → AnalysisResult.graphics_env에 저장 (항상)

fault_module이 주입 DLL 목록에 포함?
  → YES: GraphicsInjectionDiag::Diagnose()
    → GraphicsDiagResult → Evidence + Recommendations
  → NO: 환경 정보만 기록, 진단 없음
```

---

## Phase 4B: ESL/BEES/Missing Masters 진단

### 목표
Helper가 캡처 시 플러그인 메타데이터를 수집하여, dump_tool이 ESL 호환성과 Missing Masters를 자동 진단

### 데이터 수집 (Helper 확장)

#### 1. 게임 경로 획득
```cpp
// Helper에서 PID → EXE 경로 → Data 폴더
HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
wchar_t exePath[MAX_PATH];
DWORD size = MAX_PATH;
QueryFullProcessImageNameW(hProcess, 0, exePath, &size);
// exePath: "C:\...\SkyrimSE.exe"
// dataPath: parent(exePath) / "Data"
```

#### 2. MO2 감지 전략
```
usvfs_x64.dll이 모듈 목록에 있는가?
├─ NO → 표준 경로에서 수집
│   plugins.txt: %LOCALAPPDATA%/Skyrim Special Edition/plugins.txt
│   .esp 파일: <게임 EXE 디렉토리>/Data/
│
└─ YES (MO2 감지됨) →
    ├─ 게임 EXE 근처에 ModOrganizer.ini 탐색 (portable MO2)
    ├─ ini에서 active profile → profiles/<name>/plugins.txt
    ├─ 각 플러그인 실제 경로: mods/<modname>/ 에서 탐색
    └─ 못 찾으면 → "MO2 detected, plugin scan limited" 기록
        표준 경로 폴백 시도
```

#### 3. plugins.txt 파싱
```
# 형식: '*'으로 시작하면 활성
*Skyrim.esm
*Update.esm
*Dawnguard.esm
*SomeInactiveMod.esp
*NewMod.esl
```

#### 4. TES4 헤더 파싱 (최소 구현)
각 활성 플러그인의 첫 레코드(TES4)만 읽기:
- **레코드 타입**: 4바이트 "TES4"
- **데이터 크기**: 4바이트
- **플래그**: 4바이트 (bit 9 = ESL 플래그, 0x0200)
- **헤더 버전**: float (offset +20, 4바이트) — 0.94, 1.70, 1.71 등
- **MAST 서브레코드**: 마스터 파일 목록 (타입 "MAST", 가변 길이 문자열)

### 신규 파일

#### `helper/src/PluginScanner.h`

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
    float header_version = 0.0f;   // 0.94, 1.70, 1.71
    bool is_esl = false;           // ESL flag (0x0200)
    bool is_active = false;
    std::vector<std::string> masters;
};

struct PluginScanResult
{
    std::string game_exe_version;
    std::string plugins_source;     // "standard", "mo2_profile", "fallback"
    bool mo2_detected = false;
    std::vector<PluginMeta> plugins;
    std::string error;              // 실패 시 오류 메시지
};

// 게임 EXE 경로와 모듈 목록으로부터 플러그인 정보 수집
PluginScanResult ScanPlugins(
    const std::filesystem::path& gameExePath,
    const std::vector<std::wstring>& moduleNames);

}  // namespace skydiag::helper
```

#### `helper/src/PluginScanner.cpp`

TES4 레코드 구조:
```
Offset  Size  Field
0       4     Type ("TES4")
4       4     Data size
8       4     Flags (bit 9 = ESL)
12      4     FormID
16      4     Timestamp/VC info
20      4     Header version (float)
24+     var   Sub-records (HEDR, MAST, DATA, ...)
```

MAST 서브레코드:
```
Offset  Size  Field
0       4     Type ("MAST")
4       2     Data size
6       var   Null-terminated filename string
```

#### `dump_tool/data/plugin_rules.json`

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

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `shared/SkyrimDiagShared.h` | `kMinidumpUserStream_PluginInfo` 상수 추가 |
| `helper/src/DumpWriter.cpp` | 캡처 시 PluginScanner 호출, 결과를 새 user stream으로 미니덤프에 포함 |
| `dump_tool/src/Analyzer.h` | `AnalysisResult`에 `PluginScanResult` + `vector<PluginDiagResult>` 추가 |
| `dump_tool/src/Analyzer.cpp` | 새 user stream 읽기, 플러그인 규칙 매칭 |
| `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp` | Missing Masters/BEES를 Evidence 카드에 추가 |
| `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp` | 플러그인 관련 권장사항 |
| `dump_tool/src/OutputWriter.cpp` | JSON에 `plugin_scan` + `plugin_diagnosis` 섹션 |
| `dump_tool/CMakeLists.txt` | 신규 소스 파일 등록 |
| `helper/CMakeLists.txt` | PluginScanner 등록 |
| `tests/CMakeLists.txt` | 신규 테스트 등록 |

### 미니덤프 User Stream 구조

```
Stream ID: kMinidumpUserStream_PluginInfo (기존 kMinidumpUserStream_Blackbox + 1)
Format: UTF-8 JSON (PluginScanResult 직렬화)
Size: 가변 (플러그인 수에 비례, 보통 10-100KB)
```

dump_tool 읽기:
```cpp
void* pluginPtr = nullptr;
ULONG pluginSize = 0;
if (ReadStreamSized(dumpBase, dumpSize,
                    skydiag::protocol::kMinidumpUserStream_PluginInfo,
                    &pluginPtr, &pluginSize) &&
    pluginPtr && pluginSize > 0)
{
    std::string json(static_cast<const char*>(pluginPtr), pluginSize);
    out.plugin_scan = ParsePluginScanResult(json);
    // 규칙 매칭...
}
```

---

## 테스트 전략

### Phase 4A 테스트

| 테스트 파일 | 유형 | 검증 내용 |
|------------|------|----------|
| `tests/graphics_injection_rules_tests.cpp` | 정적 가드 | JSON 존재, 필수 규칙 포함, 필드 검증 |
| `tests/graphics_injection_match_tests.cpp` | 소스 가드 | Analyzer에 그래픽 진단 통합 확인 |

### Phase 4B 테스트

| 테스트 파일 | 유형 | 검증 내용 |
|------------|------|----------|
| `tests/plugin_scanner_tests.cpp` | 단위 테스트 | TES4 헤더 파싱 (테스트용 미니 .esp 바이너리) |
| `tests/plugin_rules_tests.cpp` | 정적 가드 | JSON 규칙 존재, BEES/Masters 규칙 포함 |
| `tests/plugin_stream_tests.cpp` | 소스 가드 | 새 user stream이 DumpWriter에 통합 확인 |
| `tests/data/test_plugin.esp` | 테스트 데이터 | 최소 TES4 레코드 (32바이트) |

### 테스트용 미니 ESP 바이너리 구조

```
Bytes 0-3:   "TES4" (레코드 타입)
Bytes 4-7:   0x10 0x00 0x00 0x00 (데이터 크기: 16)
Bytes 8-11:  0x00 0x02 0x00 0x00 (플래그: ESL set)
Bytes 12-15: 0x00 0x00 0x00 0x00 (FormID)
Bytes 16-19: 0x00 0x00 0x00 0x00 (VC info)
Bytes 20-23: 0x00 0x00 0xDA 0x3F (헤더 버전: 1.71f)
Bytes 24-27: "HEDR" (서브레코드)
Bytes 28-29: 0x0C 0x00 (HEDR 크기)
Bytes 30-41: HEDR 데이터 (12바이트)
```

---

## 성공 기준

| 지표 | Phase 4A 목표 | Phase 4B 목표 |
|------|-------------|-------------|
| ENB/ReShade 감지율 | 95%+ | N/A |
| 플러그인 스캔 성공률 | N/A | 표준 설치 90%+, MO2 70%+ |
| Missing Masters 감지율 | N/A | 스캔 성공 시 100% |
| BEES 필요성 판정 | N/A | 100% (헤더 1.71 존재 + 구버전 + no BEES) |
| 기존 테스트 회귀 | 0건 | 0건 |
| 신규 테스트 | +2건 | +3건 |

---

## 구현 순서 및 의존성

```
기존 Phase 1~3 (분석 정확도)
  └─ Phase 4A: ENB/ReShade/주입 DLL 진단 (dump_tool만)
       └─ Phase 4B: ESL/BEES/Missing Masters (helper + dump_tool)
```

- Phase 4A는 Phase 1의 시그니처 DB 패턴을 재사용 → Phase 1 이후 구현
- Phase 4B는 helper 수정 필요 → Windows 빌드 환경 필수
- Phase 4B의 TES4 파서는 최소 구현 (첫 레코드만, 전체 파싱 불필요)

## Codex 위임 시 유의사항

1. **Phase 4A 먼저**: dump_tool만 수정, Linux 테스트 가능
2. **Phase 4B는 Windows 빌드 필요**: helper 수정은 WSL에서 크로스컴파일 또는 Windows 네이티브 빌드
3. **TES4 파서 YAGNI**: 전체 레코드 파싱 불필요, 헤더 + MAST만 추출
4. **MO2 폴백**: MO2 경로를 못 찾아도 실패하지 않고 한계 명시
5. **기존 테스트 회귀 금지**: 변경 후 반드시 `ctest --test-dir build-linux-test --output-on-failure` 통과

## 참고 자료

- [보고서](../스카이림%20모드용%20고정밀%20크래시·프리징%20로거%20설계%20및%20구현%20분석%20보고서.md)
- [기존 설계](2026-02-15-analysis-reliability-improvement-design.md)
- [기존 구현 계획](2026-02-15-analysis-reliability-implementation-plan.md)
- [TES4 레코드 포맷](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format/TES4)
- [BEES (Backported Extended ESL Support)](https://www.nexusmods.com/skyrimspecialedition/mods/106441)
