# Prerelease Release Notes Template Design

**Date:** 2026-03-13

**Goal:** 이후 prerelease를 만들 때 GitHub Release notes 형식이 흔들리지 않도록, repo 안에 고정 템플릿과 작성 흐름을 둔다.

## Context

현재 prerelease 노트는 `gh release create/edit`를 직접 실행할 때 임시 본문으로 작성되고 있다.

문제:

- 릴리즈마다 형식이 달라진다.
- 너무 짧거나, 반대로 커밋 나열에 가까워질 수 있다.
- `rc14`처럼 나중에 다시 본문을 손봐야 하는 일이 생긴다.

반면 현재 프로젝트는 이미 다음 운영 규칙이 있다.

- GitHub Release patch notes는 한국어 우선
- zip 파일명은 `Tullius_ctd_loger_v{버전}.zip`
- release gate / package / build 절차는 문서화돼 있음

부족한 것은 **릴리즈 노트 형식 자체**다.

## Decision

### 1. repo 안에 prerelease 노트 템플릿 파일을 고정한다

고정 위치:

- `docs/release/PRERELEASE_NOTES_TEMPLATE.md`

이 파일은 실제 릴리즈 노트 본문을 복사해서 쓰는 원본이다.

목적:

- 형식 일관성 확보
- 누락 방지
- 릴리즈 직전 급한 수작업 축소

### 2. 형식은 `rc14` 구조를 기준으로 고정한다

템플릿 섹션:

- `## 핵심 변경`
- `## WinUI / 사용성`
- `## 엔진 변경`
- `## 빌드 / 운영`
- `## 주의사항`
- `## 검증`

이 구조를 선택한 이유:

- 사용자 관점과 개발자 관점을 함께 담을 수 있음
- 커밋 나열보다 읽기 좋음
- prerelease에서 “무엇을 봐야 하는지”를 빠르게 전달함

### 3. 템플릿은 빈 껍데기가 아니라 작성 규칙을 포함해야 한다

각 섹션은 제목만 두지 않고, 아래 내용을 포함한다.

- 어떤 종류의 변경을 적는지
- 적지 말아야 할 것
- 짧은 bullet 예시

즉 단순 양식이 아니라 **작성 가이드가 포함된 템플릿**이어야 한다.

### 4. 실제 릴리즈는 `--notes-file` 흐름으로 고정한다

개발 문서에 다음 흐름을 명시한다.

1. 템플릿을 복사
2. 버전별 초안 파일 작성
3. `gh release create/edit --notes-file ...`

이렇게 해야:

- 임시 CLI 인라인 본문 사용을 줄일 수 있음
- 릴리즈 본문이 shell history에 묻히지 않음
- 필요 시 draft 파일을 리뷰할 수 있음

### 5. 자동 생성까지는 이번 단계 범위에 넣지 않는다

이번 단계 비범위:

- 커밋 로그 자동 요약 스크립트
- CHANGELOG 자동 동기화
- semantic-release / standard-version

현재 목표는 **형식 고정**이지, **자동화 파이프라인 구축**이 아니다.

## Architecture

### Files

- Create: `docs/release/PRERELEASE_NOTES_TEMPLATE.md`
- Modify: `docs/DEVELOPMENT.md`
- Optional test guard: `tests/packaging_includes_cli_tests.py`

### Template behavior

템플릿은 다음 특성을 가져야 한다.

- 한국어 기본
- 섹션 고정
- bullet 구조 고정
- release-specific placeholders 포함
  - 버전
  - 주요 기능
  - opt-in/주의사항
  - 검증 결과

### Operational flow

권장 흐름:

1. `cp docs/release/PRERELEASE_NOTES_TEMPLATE.md docs/release/drafts/<version>.md`
2. 내용 채우기
3. `gh release create ... --notes-file docs/release/drafts/<version>.md`
4. 또는 `gh release edit ... --notes-file ...`

## Risks And Mitigations

### 1. 템플릿이 너무 길어질 위험

대응:

- 섹션 수는 고정
- 각 섹션 bullet 2~4개 수준 권장
- 커밋 나열 금지 규칙 포함

### 2. 문서만 생기고 실제로 안 쓰일 위험

대응:

- `docs/DEVELOPMENT.md` release section에 명시
- 필요하면 guard test로 템플릿 존재와 문서 참조를 고정

### 3. prerelease와 정식 release 형식이 충돌할 위험

대응:

- 파일명을 `PRERELEASE_NOTES_TEMPLATE.md`로 명확히 제한
- 정식 릴리즈 템플릿은 별도 문제로 남김

## Non-goals

- 정식 릴리즈 노트 포맷 설계
- changelog 자동 생성
- GitHub Actions release automation

## Acceptance Criteria

- repo 안에 prerelease 릴리즈 노트 템플릿 파일이 생긴다.
- `docs/DEVELOPMENT.md`가 `--notes-file` 사용 흐름을 설명한다.
- 템플릿이 `rc14` 구조를 기준으로 섹션과 작성 규칙을 고정한다.
