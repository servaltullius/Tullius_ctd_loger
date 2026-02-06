# ADR-0004: SharedLayout Versioning and Compatibility Policy

## Status
Accepted

## Context
`SkyrimDiag`는 플러그인(`SkyrimDiag.dll`)과 헬퍼(`SkyrimDiagHelper.exe`)가
공유 메모리(`SharedLayout`) 프로토콜로 강하게 결합되어 동작합니다.

현재 구현은 `magic/version`이 맞지 않으면 헬퍼가 즉시 attach를 중단(fail-fast)합니다.
- shared layout version: `shared/SkyrimDiagShared.h` (`kVersion`)
- helper mismatch handling: `helper/src/main.cpp`

이 동작은 안전하지만, 운영 정책이 문서화되지 않으면 다음 위험이 있습니다.
- 부분 업데이트(플러그인만 교체, 헬퍼만 구버전 유지)
- MO2에서 구버전 파일 잔존으로 인한 예측 불가 진단 실패
- 필드 확장 시 호환성 기준 불명확

## Decision
다음 정책을 공식 채택합니다.

1. **기본 원칙: fail-fast 유지**
- `magic/version` 불일치 시 헬퍼는 진단을 진행하지 않고 종료한다.
- 잘못된 덤프 생성보다 명시적 실패가 안전하므로 현 정책을 유지한다.

2. **버전 증가 규칙**
- `SharedLayout`의 메모리 레이아웃/의미가 바뀌면 `kVersion`을 반드시 증가시킨다.
- 아래 변경은 버전 증가가 필수다.
  - 구조체 필드 추가/삭제/타입 변경
  - 기존 필드의 의미/단위 변경
  - 이벤트/리소스 버퍼 해석 규칙 변경

3. **릴리스 번들링 규칙**
- 플러그인 + 헬퍼 + DumpTool + ini를 항상 같은 릴리스 zip으로 배포한다.
- 부분 배포(개별 바이너리 교체)는 공식 지원하지 않는다.

4. **확장 전략(향후)**
- 경미한 확장은 가능한 경우 `reserved`/신규 tail field로 추가하고, 의미가 모호하면 버전을 올린다.
- 상호 운용이 필요해지면 `FeatureFlags` 도입을 검토하되,
  초기 단계에서는 단순하고 명확한 fail-fast 정책을 우선한다.

## Consequences

### Positive
- 프로토콜 불일치로 인한 잘못된 진단/오탐 가능성을 낮춘다.
- 사용자/테스터에게 실패 원인이 명확해진다(버전 mismatch 로그).
- 릴리스/패키징 정책이 일관된다.

### Negative
- 구버전/신버전 혼합 환경에서 자동 하위 호환은 제공하지 않는다.
- 사용자 입장에서는 "전체 zip 재설치"가 필요할 수 있다.

### Neutral
- 향후 복잡한 협상형 호환(`feature negotiation`)을 도입하려면 추가 ADR이 필요하다.

## Operational Guidance
- 릴리스 체크리스트에 아래를 포함한다.
  - `kVersion` 변경 여부 검토
  - mismatch 로그 문구 유지/점검
  - package zip 내 구성품 동시 갱신 확인
- 사용자 안내 문서에는 "부분 업데이트 대신 전체 재설치"를 권장한다.

## References
- `shared/SkyrimDiagShared.h`
- `helper/src/main.cpp`
- `scripts/package.py`
- `docs/adr/0001-out-of-proc-helper-and-shared-memory-blackbox.md`
