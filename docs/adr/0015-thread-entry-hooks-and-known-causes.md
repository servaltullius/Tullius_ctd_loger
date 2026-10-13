# ADR-0015: Thread-Entry Hooks and Known-Cause Signatures

## Status
Accepted

## Context
2026-10-13에 v0.2.62를 설치한 사용자 환경(1.6.1170, MO2)에서 시작 85초 만에 CTD가 났습니다. 리포트는 `NPCWaterAIFix.dll`을 중간 신뢰도 후보로 지목했지만, 실제 원인은 그 DLL이 아니었습니다.
- 예외: Crash Logger가 처리되지 않은 예외로 기록한 C++ `std::invalid_argument` "invalid stoull argument"
- 예외를 던진 곳: 게임 EXE 안쪽의 게임 프레임 10개(AE ID 442xxx~443xxx)가 MSVCP140의 `stoull`을 불렀습니다.
- 원인: 같은 PC에서 Steam 1.7.104를 실행하자, 두 버전이 함께 쓰는 `%LOCALAPPDATA%\Skyrim Special Edition\ContentCatalog.txt`가 1.7 형식으로 다시 쓰였습니다(키 `CSV2_<uuid>`, 버전 `"150674.== Version Number =="`). 1.6.1170은 이 파일을 읽다가 예외를 냈습니다. 크래시 지점에는 `InstalledContent*` 오브젝트가 있었습니다. 파일을 휴지통으로 옮기자 정상 실행됐습니다.
- `NPCWaterAIFix.dll`과 `Trails.dll`은 게임 초기화 스레드(`InitTESThread`)의 시작 지점을 훅하고 있었습니다. 그 아래에는 게임의 스레드 시작 루틴(`StartAddress`) 하나와 Windows 스레드 시작 코드만 있었습니다.
- 그런데도 우리 콜스택 점수와 Crash Logger의 "첫 유력 DLL 프레임"·"상위 모듈" 가산점이 이 훅을 후보로 올렸습니다.

## Decision
1. **스레드 입구 훅**(`dump_tool/src/ThreadEntryHooks.h`)
   - 판정 조건: 콜스택에서 연속된 플러그인 프레임 묶음 위로(크래시 쪽) 게임 프레임이 3개 이상 바로 이어지고, 아래로는 게임 프레임이 정확히 1개(게임의 스레드 시작 루틴) 있고 그다음은 시스템 프레임만 스택 끝까지 이어질 것
   - 이 조건을 만족하는 묶음은 그 스레드가 시작된 곳일 뿐 원인이 아닌 것으로 봅니다.
   - 판정하지 않는 경우
     - 플러그인 자신의 스레드: 아래에 게임 프레임이 없습니다.
     - 게임이 부른 플러그인 코드: 아래에 게임 프레임이 여러 개 있습니다(크래시 테스트의 엔진 오류도 여기에 해당).
     - 메인 스레드: 맨 아래에 게임 프레임이 2개(WinMain 루프, CRT 시작) 있습니다.
     - 스택 해제가 스레드 시작까지 닿지 않은 경우
   - 적용 위치
     - 우리 콜스택 점수
     - Crash Logger의 첫 유력 DLL, 연속 프레임, 상위 모듈(`CrashLoggerParseCore`, `CrashLogger.cpp`)
     - 스택 스캔 대체 후보
   - 정식 스택 해제가 스레드 시작까지 끝났고 플러그인 프레임이 모두 이런 훅이면, 포인터 스캔으로 넘어가지 않습니다. 넘어가면 Crash Logger나 MO2 VFS의 오래된 리턴 주소만 후보로 더해지기 때문입니다.
   - 리포트에 근거 항목 "게임 스레드 시작 지점의 훅(후보에서 제외)"을, 요약 JSON에 `thread_entry_hooks`를 넣습니다.
2. **알려진 원인 시그니처**
   - `crash_signatures.json`에 매칭 조건 두 가지를 더했습니다.
     - `cpp_exception_info_contains`: Crash Logger C++ 예외 메시지의 부분 문자열(대소문자 무시)
     - `object_type_any`: 크래시 근처 오브젝트 타입의 정확한 이름(대소문자 무시). Crash Logger의 POSSIBLE RELEVANT OBJECTS·REGISTERS와 덤프 오브젝트에서 가져오며, 플러그인도 이름도 없는 `InstalledContent` 같은 타입도 포함합니다.
   - 새 시그니처 `CONTENT_CATALOG_FORMAT_MISMATCH`(root_cause, 신뢰도 중간)
     - 조건: `0xE06D7363` + "invalid stoull argument" + `InstalledContent`
     - 권장 조치: `[알려진 원인]` ContentCatalog.txt를 휴지통으로 옮기기
   - `[Known cause]`·`[알려진 원인]` 태그는 다음 조치 우선순위의 맨 앞입니다. 뷰어에서는 "지금 바로" 묶음에 들어갑니다.
3. Crash Logger가 확인한 C++ 예외인데 후보가 없으면, 결론에 "잡힌 예외일 수 있음" 대신 "실제 CTD, 콜스택에 원인으로 볼 플러그인 없음"이라고 씁니다.

## Consequences
- 2026-10-13 CTD 재분석 결과
  - 결론: 알려진 근본 원인 패턴 `CONTENT_CATALOG_FORMAT_MISMATCH`
  - 다음 조치: ContentCatalog.txt를 휴지통으로 옮기기
  - 후보 0개, 두 DLL은 "스레드 시작 지점의 훅"으로 설명
- 이 판정 때문에 플러그인이 스레드 입구 훅 코드 안에서 직접 잘못된 일을 한 경우까지 후보에서 빠질 수 있습니다. 그 경우에도 크래시 프레임은 훅 위의 게임 코드에 있으므로, 이전에도 그 DLL을 지목할 직접 근거는 없었습니다.
- 시그니처 메시지와 오브젝트 타입은 1.6.1170의 한 사례에서 나왔습니다. 다른 원인으로 같은 메시지가 나고 `InstalledContent`가 근처에 있으면 오진할 수 있어 신뢰도는 중간으로 두었습니다.

## Verification
- `skydiag_thread_entry_hook_tests`
  - 이번 CTD의 스택 모양
  - 판정하지 않는 경우: 자기 스레드, 메인 스레드, 깊은 게임 호출, 짧은 위쪽, 끊긴 스택
  - 실제 Crash Logger 로그 발췌의 첫 유력 DLL, 훅 목록, 상위 모듈
  - 오브젝트 타입 파서
- `skydiag_analysis_engine_runtime_tests`: 번들 DB의 `CONTENT_CATALOG_FORMAT_MISMATCH` 일치·불일치, 새 조건의 잘못된 항목 거부, 정확한 타입 이름 비교
- `skydiag_next_action_tests`: `[알려진 원인]`이 후보 줄과 modal 줄보다 먼저 선택됨
- 보관 덤프 19개(크래시 10, 프리징 9)를 v0.2.62 분석기와 나란히 재분석했습니다. 결론, 버킷, 추정 모드, 다음 조치, 후보가 바뀐 것은 2026-10-13 CTD 1건뿐입니다.

## Addendum (2026-10-13): known crash sites and a cut-short call stack

- **알려진 크래시 지점 시그니처**: Codex(gpt-6.1-sol) 조사 후보 29개 중 근거를 직접 확인한 것만 넣었습니다. 모두 scope는 `mechanism`이고, 권장 조치 태그는 `[알려진 크래시 지점]`입니다. 이 태그는 다음 조치 최우선은 아니지만, 뷰어에서는 "지금 바로" 묶음에 들어갑니다.
  - `FACEGEN_MORPHDATA_NULLPTR_*`, `SHADOWSCENENODE_NULLPTR_*`: 1.5.97, 1.6.640, 1.6.1170
    - Engine Fixes 소스(`RELOCATION_ID(26343, 26918)+0x4C`, `RELOCATION_ID(99708, 106342)+0x16`)에서 ID, 오프셋, 명령어를 확인했습니다.
    - 버전별 주소는 Address Library로 구했습니다. 1.6.1170은 실제 exe의 `.pdata` 함수 시작과도 맞췄습니다.
    - 1.7.104는 Engine Fixes 지원을 확인하지 못해 뺐습니다.
  - `WATER_REFLECTIONS_ADDWATER_1_6_1170`: 공개 조사 문서의 `AddWater+0x697`을 실제 exe 함수 시작 `0x523280`과 맞췄습니다.
  - `NITRISHAPE_MISSING_GEOMETRY_1_7_104`: 문서의 `SkyrimSE+EDBD16`을 씁니다. 두 번째 주소는 확인하지 못해 뺐습니다.
  - `ANIMATION_CLIPGENERATOR_A0D789_1_5_97`
  - `CPP_BAD_ALLOC`: 메시지와 `std::bad_alloc` 타입이 둘 다 있어야 합니다.
  - 보류: Havok 3종. 근거가 공개된 지 며칠 된 모드 설명 하나뿐입니다.
  - 보류: 새 신호가 필요한 18종(마스터 누락, Address Library 누락, ESL 한도, 레벨드 리스트 등). 조사 원문은 측정 세트 폴더의 `research/`에 있습니다.
  - 일반 메커니즘(`ACCESS_VIOLATION_NULL` 등)보다 앞에 두어, 정확한 지점이 먼저 일치하게 했습니다.
- **콜스택 끊김**
  - 대표 스레드의 스택 해제가 스레드 시작 전에 멈추면 근거 항목과 요약 JSON `stackwalk_truncation`에 이유를 적습니다. 이유는 `outside_modules`, `non_code_address`, `no_unwind_info` 중 하나입니다.
  - 덤프의 언로드된 모듈 목록으로, 끊긴 주소나 예외 주소가 언로드된 DLL 자리면 그 이름을 적습니다.
  - Windows DLL의 해제 정보가 없으면 "v0.2.61 이전 덤프를 Windows 업데이트 뒤 열 때 흔함"을 덧붙입니다.
- **확인**
  - 보관 덤프 24개를 v0.2.62와 나란히 비교했습니다. 바뀐 것은 이미 머지한 #96·#99로 설명되는 2건뿐이고, 새 시그니처가 잘못 일치한 덤프는 없습니다.
  - 끊김은 이전 실사고 프리징 6건(Windows DLL 해제 정보 없음)과 CTD 1건(데이터 주소)에서만 나왔습니다.
