# ADR-0013: Problematic Modules

## Status
Accepted

## Context
어떤 SKSE 플러그인은 로드되어 있기만 해도 크래시 근거를 믿기 어렵게 만듭니다. SkyrimCrashGuard는 예외를 가로채 게임을 계속 실행하려 합니다.

- 게임 상태가 망가진 채로 계속 돌고, 원래 원인이 숨거나 나중의 다른 크래시·프리징으로 바뀝니다.
- 벡터 예외 처리기(VEH)를 가로채서 Crash Logger와 이 로거의 플러그인(역시 VEH)이 크래시를 아예 못 볼 수 있습니다.

Crash Logger(MIT)는 `src/Crash/ProblematicModules.cpp`에서 이 DLL을 파일 이름으로 찾아 경고합니다. 2026-10 기준 그 목록에는 SkyrimCrashGuard 하나만 있습니다.

## Decision
1. 분석기는 덤프의 모듈 목록에서 문제 모듈을 파일 이름 정확 일치(대소문자 무시)로 찾는다. 목록은 `dump_tool/src/ProblematicModules.cpp`의 표이며, 근거가 분명한 항목만 출처 링크와 함께 더한다.
2. 찾으면 캡처 종류(크래시, 프리징, 수동, modal 대화상자 프리징)와 상관없이 다음을 낸다.
   - 근거 항목(신뢰도 중간): 모듈, MO2 모드 이름, 이유, 모드 페이지
   - `[문제 모듈]` 권장 조치: 원인을 찾는 동안 비활성화하고 다시 재현
   - 리포트의 `문제 모듈:` 줄, 요약 JSON의 `problematic_modules`
3. 후보, 버킷, 이력에는 영향을 주지 않는다. 문제 모듈은 원인이 아니라 근거의 신뢰도에 관한 것이기 때문이다.

## Consequences
- 보관 덤프 17개에는 해당 모듈이 없어 결과가 바뀌지 않는다.
- 이름이 바뀐 DLL이나 다른 이름으로 같은 일을 하는 모듈은 찾지 못한다.
