# Prerelease Release Notes Template

> 한국어 우선으로 작성합니다. 커밋 나열 대신, 사용자가 체감하는 변경과 주의사항을 먼저 적습니다.
>
> 사용 방법:
> 1. 이 파일을 버전별 초안으로 복사합니다.
> 2. 섹션별 bullet 2~4개 수준으로 채웁니다.
> 3. `gh release create ... --notes-file <draft>` 또는 `gh release edit ... --notes-file <draft>`로 업로드합니다.

## 핵심 변경
- 사용자 관점에서 가장 중요한 변경 2~4개를 적습니다.
- CTD/freeze 정확도, 핵심 워크플로우, 결과 해석 변화처럼 체감되는 변화 위주로 적습니다.
- 내부 리팩터링이나 문서 추가만 나열하지 않습니다.

예시:
- EXE / system-victim CTD에서 행동 우선 후보를 더 정확히 끌어올리도록 분석 로직을 강화했습니다.
- freeze / ILS 분석이 `deadlock`, `loader stall`, `ambiguous freeze`를 더 구체적으로 구분합니다.

## WinUI / 사용성
- WinUI, triage, dump intake, 공유 흐름, empty state 등 사용성 변화를 적습니다.
- 화면 위치나 사용 순서가 바뀌었으면 여기서 설명합니다.
- 순수 엔진 내부 변경은 여기 넣지 않습니다.

예시:
- 권장 조치 영역에 recapture context를 추가해, richer/full/snapshot 재수집 이유를 UI에서 직접 볼 수 있습니다.
- 시작 화면은 최근 발견된 덤프와 출력 위치 중심으로 동작합니다.

## 엔진 변경
- capture profile, analyzer, blackbox, symbol/runtime, snapshot, recapture policy 같은 엔진 변화를 적습니다.
- opt-in 기능은 기본값 상태까지 함께 적습니다.

예시:
- `DumpMode` 단순 3단 구조를 incident-aware capture profile 기반으로 보강했습니다.
- freeze/manual capture에는 opt-in `PSS snapshot` 경로를 추가했습니다. 기본값은 여전히 `EnablePssSnapshotForFreeze=0`입니다.

## 빌드 / 운영
- 빌드 스크립트, 패키징, 릴리즈 게이트, 운영/배포 관련 변경을 적습니다.
- 개발자/테스터가 바로 알아야 하는 실행 방식 변경이 있으면 여기에 적습니다.

예시:
- WSL에서 Windows 빌드를 안정적으로 돌릴 수 있도록 `build-win-from-wsl.sh`, `build-winui-from-wsl.sh` 래퍼를 추가했습니다.
- 릴리즈 게이트와 패키지 계약을 최신 구조에 맞게 갱신했습니다.

## 주의사항
- opt-in 기능, fallback 제거, 호환성 제약, 해석 범위처럼 사용자가 알아야 할 제한사항을 적습니다.
- “무엇이 아직 기본값이 아닌지”, “무엇을 자동으로 하지 않는지”를 명확히 적습니다.

예시:
- `PSS snapshot` 기반 freeze capture는 아직 opt-in 기능입니다.
- generic Windows `CrashDumps`는 자동 발견 대상이 아니며, 필요 시 직접 선택으로만 여는 흐름을 권장합니다.

## 검증
- 이번 prerelease를 만들 때 실제로 돌린 검증만 적습니다.
- 이 저장소에서는 로컬 검증 결과를 우선 적습니다. GitHub Actions는 선택적 참고 사항입니다.
- 가능한 한 명령이 아니라 결과를 적습니다.

예시:
- Linux test suite: `53/53` 통과
- Windows native build: 성공
- Windows WinUI build: 성공
- CLI smoke (`SkyrimDiagDumpToolCli.exe --help`): 정상
