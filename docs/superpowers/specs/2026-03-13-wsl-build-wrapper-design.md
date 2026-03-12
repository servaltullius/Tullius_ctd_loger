# WSL Windows Build Wrapper Design

**Goal:** WSL 사용자도 상대 경로/UNC current directory 문제 없이 Windows 빌드 스크립트를 안정적으로 실행할 수 있게 한다.

**Scope**
- `scripts/build-win.cmd`, `scripts/build-winui.cmd`를 감싸는 WSL 전용 bash 래퍼 2개 추가
- repo `AGENTS.md` BUILD GUIDE에 WSL 호출 진입점 추가
- `docs/DEVELOPMENT.md`에 WSL 호출 예시와 제한 사항 문서화
- source-guard 테스트로 래퍼와 문서 계약 고정

**Non-goals**
- `cmd.exe /c scripts\build-win.cmd` 같은 WSL 상대 경로 호출 자체를 복구하지 않는다
- Windows 네이티브 사용자의 기본 진입점은 유지한다
- release gate 대상 스크립트 집합은 확대하지 않는다

**Approach**
- 래퍼는 WSL/bash에서 실행된다고 가정하고, 자신의 `.cmd` 형제 파일 절대 경로를 계산한 뒤 `wslpath -w`로 Windows 절대 경로로 변환한다.
- Windows 쪽 호출은 `powershell.exe -NoProfile -Command "& '<abs>'"` 형태로 통일한다.
- 비-WSL 또는 필수 도구(`wslpath`, `powershell.exe`) 부재 시 즉시 실패하게 해 오동작을 숨기지 않는다.

**Validation**
- source-guard 테스트가 래퍼 존재, `wslpath -w`, `powershell.exe`, BUILD GUIDE/DEVELOPMENT 문서 반영 여부를 검증한다.
- Linux 전체 테스트와 Windows 빌드 래퍼 실제 호출을 확인한다.
