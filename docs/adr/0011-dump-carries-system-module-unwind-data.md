# ADR-0011: Dumps Carry Windows Modules' Unwind Data and Export Names

## Status
Accepted

## Context
2026-10-11에 이 PC의 Windows가 업데이트되었습니다(ntdll 10.0.26100.9549). 그 뒤 보관 중인 실사고 프리징 덤프 5건을 다시 분석하자 모두 결론이 바뀌었습니다.

- 메인 스레드 스택이 ntdll 프레임에서 끊겼습니다.
- 대기 분류(ADR-0009)는 `unknown`이 되었고, 리포트는 예전처럼 스택 스캔으로 EngineFixes·FSMP를 후보로 올렸습니다.
- ntdll 프레임에는 `ntdll.dll!RtlQueryFeatureConfiguration+0xcb4`처럼 틀린 이름이 붙었습니다.

원인은 두 가지입니다.

- 스택 해제에 필요한 `.pdata`와 UNWIND_INFO를, 덤프에 없으면 같은 빌드(TimeDateStamp, SizeOfImage)의 로컬 파일에서만 읽습니다(ADR-0007). 업데이트 뒤에는 맞는 로컬 파일이 없습니다.
- DbgHelp는 버전이 다른 로컬 파일도 그대로 불러와 그 파일의 export와 PDB로 이름을 붙였습니다.

덤프 종류에 따라 상황이 다릅니다.

- 크래시 덤프는 code segments·module headers 설정 덕분에 모듈 헤더, `.pdata`, export 디렉터리를 이미 담고 있었습니다.
- 프리징·수동 덤프는 모듈 헤더조차 담지 않았습니다.
- 온라인 심볼을 허용해도 실행 파일을 받아 오지 않아 회복되지 않았습니다.

캡처 직후의 분석은 같은 DLL이라 영향이 없습니다. 문제는 업데이트 뒤나 다른 PC에서 이전 덤프를 다시 볼 때입니다.

## Decision

1. 헬퍼는 모든 덤프(크래시, 프리징, 수동)에 Windows 디렉터리 아래 모듈의 다음 범위를 추가한다. 대상은 시스템 DLL과 DriverStore의 그래픽 드라이버이며, Windows나 드라이버 업데이트가 바꾸는 파일이다.
   - PE 헤더
   - export 디렉터리
   - exception 디렉터리(`.pdata`)
   - `.pdata`가 가리키는 UNWIND_INFO 각각. 크기는 unwind code 수(짝수로 맞춤)와 handler RVA, chain된 RUNTIME_FUNCTION으로 계산하고, chain을 따라간다.

   읽는 것은 실행 중인 게임 프로세스이고, ADR-0010과 같은 `MemoryCallback`으로 덤프에 추가한다. 상한은 24MB다(플러그인 약 4,250개 환경에서 약 17MB). 게임, SKSE, 모드 파일은 넣지 않는다. PSS 스냅샷 핸들은 프로세스 핸들이 아니어서 그 경로의 덤프는 제외한다. PSS 스냅샷은 기본값이 꺼져 있다.
2. 분석기는 로컬 파일이 덤프의 모듈과 같은 빌드인지 먼저 확인한다.
   - 같지 않거나 파일이 없으면 그 파일을 DbgHelp에 넘기지 않고 빈(virtual) 모듈로 등록한다.
   - 이름은 덤프의 export 디렉터리에서 붙인다. 각 이름의 범위는 다음 export까지이며 0x2000바이트를 넘지 않는다. 표시용 이름은 export 이름과 같은 0x1000 거리 제한을 받는다.
3. 스택 해제의 함수 테이블 조회 순서는 다음과 같다.
   - 같은 빌드의 로컬 파일
   - 덤프 안의 `.pdata`(이진 탐색, `.pdata` 간접 엔트리 처리)
   - DbgHelp

   UNWIND_INFO는 원래대로 덤프 메모리에서 먼저 읽는다.
4. `SKYDIAG_STACKWALK_IGNORE_LOCAL_IMAGES=1`은 모든 로컬 파일을 다른 빌드로 취급한다. 테스트 전용이며, 덤프만으로 분석되는지 확인하는 데 쓴다.

## Consequences

### Positive

- 업데이트 뒤나 다른 PC에서도, 이 버전 이후의 프리징·수동 덤프는 시스템 DLL 프레임을 지나 스택을 풀고 대기 API 이름을 붙인다. 따라서 대기 분류(ADR-0009)와 modal 판정(ADR-0006)이 유지된다.
- 버전이 다른 로컬 파일 때문에 틀린 함수 이름이 붙지 않는다.

### Limitations

- 이 버전 이전의 프리징·수동 덤프에는 이 데이터가 없어, 업데이트 뒤에는 여전히 ntdll에서 끊긴다.
- 게임 EXE와 모드 DLL은 대상이 아니다. 모드를 업데이트한 뒤 이전 덤프를 보면 그 모드 프레임에서 끊길 수 있다.
- 덤프가 프리징·수동 기준 약 17MB(약 3%) 커지고, 캡처마다 모듈 데이터를 읽는 시간이 든다.

## Verification

- `skydiag_modal_dialog_hang_e2e_dump_only_tests`: 실제 헬퍼 프리징 캡처를 모든 로컬 파일이 다른 빌드인 것처럼 분석해도 MessageBox 대기 체인과 호출 모듈을 찾는다. 헬퍼가 모듈 데이터를 담지 않으면 "Main-thread stack must match the MessageBox wait chain"으로 실패한다.
- `skydiag_crash_dump_context_tests`: UNWIND_INFO 크기(짝수 패딩, handler, chain, chain 대상).
- 보관 덤프 재분석: CTD 5건의 결론, NextAction, 버킷, 콜스택은 변화 없음.

## References

- `helper/src/ModuleUnwindMemory.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalkDumpImage.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalkSymbols.cpp`
- `docs/adr/0007-formal-stackwalk-unwinding.md`
