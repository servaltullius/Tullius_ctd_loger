# ADR-0007: Formal Stackwalk Unwinding

## Status
Accepted

## Context
초기 구현부터 정식 stackwalk 루프는 컨텍스트의 Rip을 기록한 뒤 `StackWalk64`를
호출하고, 반환된 PC가 같으면 멈췄습니다. `StackWalk64`의 첫 호출은 컨텍스트 자신의
프레임을 그대로 돌려주므로 루프는 항상 1프레임에서 끝났습니다.

- CTD는 fault 프레임 하나만 점수에 쓰였고, fault가 시스템 DLL이나 게임 EXE이면
  suspect가 비어 포인터 스캔으로 넘어갔습니다.
- 프리징은 사실상 항상 "정식 stack walking 실패 → 메인 스레드 포인터 스캔"이었습니다.

루프만 고치면 다른 문제가 드러났습니다. 덤프 분석에서 DbgHelp의
`SymFunctionTableAccess64`는 대상 프로세스 메모리에서 `.pdata`를 읽으려 하므로 항상
NULL을 돌려줍니다. 그러면 모든 함수가 leaf로 취급되어 스택 슬롯이 8바이트씩 반환 주소로
해석됩니다. 2026-10-01 실사고 덤프 7개(crash 2, hang 5)에서는 힙 주소, 전역 데이터,
vftable이 프레임으로 나타났고, 무관한 `DtryKeyUtil.dll`이 `Medium` 후보가 됐습니다.

또한 hang에서 메인 스레드 외에 WCT longest-wait 스레드 최대 8개도 stackwalk 대상이었습니다.
메인 스레드 스택에 실행 가능한 모듈이 없으면, 자기 루프에서 쉬고 있던
`ColdBreathNG.dll` 워커 스레드가 대표로 선택되어 `Medium` 후보가 됐습니다.

## Decision

1. 정식 stackwalk는 첫 `StackWalk64` 호출부터 프레임을 기록하고, unwind가 실패하거나
   PC와 SP가 함께 반복되면 멈춘다.
2. 덤프에 없는 모듈 메모리는 로컬 이미지 파일에서 읽는다. 로컬 파일의
   `TimeDateStamp`와 `SizeOfImage`가 덤프 모듈과 같을 때만 사용한다.
3. 함수 테이블 조회는 같은 검증된 로컬 이미지의 예외 디렉터리에서 직접 찾는다.
4. 0번 프레임만 leaf일 수 있다. 호출자 프레임이 알려진 모듈 밖이면 기록하지 않고,
   unwind 항목이 없으면 그 프레임까지 기록한 뒤 멈춘다. 스택 슬롯 추측으로 계속 내려가지
   않는다.
5. 메인 스레드를 아는 hang은 메인 스레드와 WCT cycle 스레드만 stackwalk한다
   (ADR-0005). 오래 기다린 워커 스레드는 정지의 근거가 아니다.
6. 프레임을 2개 이상 얻었지만 실행 가능한 모듈이 없으면 진단을
   `formal stackwalk found no actionable module`로 남긴다. helper의 재수집 상향 판단에
   쓰이는 `DbgHelp stackwalk failed`는 실제로 프레임을 얻지 못했을 때만 남긴다.
7. 버킷 입력 프레임이 달라지므로 crash bucket을 v3(`CTD3-`)로 올린다.

## Consequences

### Positive

- CTD와 hang 리포트에 실제 호출 체인이 나타나고, 사용 가능한 경우 소스 줄까지 표시된다.
- 실사고 hang 4건에서 메인 스레드가 렌더 경로를 실행 중인 것이 드러났고, 1건은 GPU
  드라이버 안에서 대기 중인 것이 프레임으로 확인됐다.
- 쓰레기 프레임과 워커 스레드 오지목을 만들지 않는다.

### Limitations

- 로컬 이미지가 없거나 버전이 다르면(삭제·업데이트된 모드, 다른 PC에서 분석) 그
  모듈에서 walk가 멈춘다.
- 기존 `CTD2-` history 그룹과 새 `CTD3-` 그룹은 자동으로 합쳐지지 않는다.
- 후킹 체인(D3D 초기화 훅 등)에 놓인 다른 DLL도 호출자 프레임으로 나타난다. 점수 가중치와
  후보 정책은 바꾸지 않았으므로 이런 DLL은 `Low` 보조 후보로만 남는다.
- 실사고 비교는 한 사용자의 덤프 7개에 대한 것이며 정확도 측정이 아니다.

## Verification

- `skydiag_stackwalk_e2e_tests`: 세 단계 호출 체인에서 대기 중인 스레드를 실제 helper hang
  캡처로 덤프하고, 정식 stackwalk가 체인 전체를 순서대로 복원하며 모듈 밖 주소가 없는지 확인한다.
- `skydiag_modal_dialog_hang_e2e_tests`: 같은 공용 walker로 modal 스택 근거가 성립한다.

## References

- `dump_tool/src/AnalyzerInternalsStackwalkMemory.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalk.cpp`
- `dump_tool/src/Analyzer.CaptureInputs.cpp`
- `dump_tool/src/Bucket.h`
- `docs/adr/0005-hang-main-thread-and-thread-group-consensus.md`
