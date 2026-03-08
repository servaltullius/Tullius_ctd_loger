# Runtime And Loader Hardening Plan

> 작성일: 2026-03-08
> 목적: helper/plugin 런타임 안정성과 dump_tool 로더/릴리스 계약 견고성을 동시에 개선

## 범위

### 1. Helper / Plugin 런타임 안정성

- duplicate helper가 활성 인스턴스 로그를 지우지 않도록 로그 초기화 순서 조정
- manual capture hotkey의 `WM_HOTKEY` + polling fallback 이중 발화 방지
- 숫자형 INI 값에 최소/최대 범위 clamp 추가

### 2. Dump tool 로더 견고성

- `AddressResolver`가 malformed entry 하나 때문에 전체 DB 로드를 실패하지 않도록 partial-load 허용
- malformed runtime 테스트 추가

### 3. 남은 개선 항목

- `TroubleshootingGuide` 로더가 `version` / 필수 타입 / match 조건을 검증하도록 강화
- headless CLI가 성공 시 summary/report 산출물 경로를 stdout에 안정적으로 출력
- release gate가 WinUI sidecar(`.runtimeconfig.json`, `.deps.json`)와 스크립트 미러 드리프트를 더 엄격하게 검사
- README 계열 문서에서 실제 WinUI 동작과 어긋나는 drag & drop 안내 정리

## 구현 순서

1. helper main / config / plugin config 수정
2. AddressResolver hardening 수정
3. TroubleshootingGuide / CLI / release contract / README 정리
4. source-guard / runtime tests 보강
5. Linux build + CTest 검증
