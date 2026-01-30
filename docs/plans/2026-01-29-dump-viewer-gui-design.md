# DumpTool GUI Viewer (2026-01-29)

## Goal

일반 사용자가 WinDbg/명령어 없이도 `.dmp`를 열고,
- “유력 후보(모드/DLL) + 신뢰도”
- “왜 그렇게 판단했는지(근거)”
- “크래시 직전 타임라인(블랙박스)”
- “프리징의 대기 체인(WCT)”
을 한 화면에서 이해하도록 한다.

## Constraints / Safety

- 덤프만으로 “원인 확정”은 대부분 불가능하므로, 결론은 **유력/가능성**으로만 표기한다.
- 자동 분석(게임 실행 중)에서는 UI 팝업이 방해가 되므로, 헬퍼는 DumpTool을 `--headless`로 실행하여 파일만 생성한다.

## Data Sources

- 표준 minidump 스트림:
  - `ExceptionStream` → 예외 코드/주소/스레드
  - `ModuleListStream` → 주소→모듈 매핑(모듈+오프셋)
- SkyrimDiag user stream:
  - `kMinidumpUserStream_Blackbox` → 이벤트 타임라인
  - `kMinidumpUserStream_WctJson` → WCT(JSON)

## UX

- `SkyrimDiagDumpTool.exe` 더블클릭 → 파일 선택 → 분석 → 뷰어 UI
- `.dmp` 드래그&드롭 → 즉시 재분석
- 탭:
  - 요약: 결론/핵심 수치/권장 점검
  - 근거: 신뢰도/판단/근거 표
  - 이벤트: 블랙박스 테이블
  - WCT: JSON 보기(추후 트리/그래프 확장 가능)

## Outputs

자동/수동 공통으로 덤프 옆에 생성:
- `*_SkyrimDiagReport.txt`
- `*_SkyrimDiagSummary.json`
- `*_SkyrimDiagBlackbox.jsonl` (가능할 때)
- `*_SkyrimDiagWct.json` (가능할 때)

