# ADR-0023: 덤프에 담는 모듈 메모리 줄이기 (게임 EXE 코드와 스택이 가리키는 코드)

## Status
Proposed (2026-10-30, 구현 전 초안)

## Context
- 2026-10-30 넥서스 피드백: 출력 폴더가 15GB였습니다. main에 들어간 `MaxTotalDumpGB`(기본 5GB, 아직 출시 전)가 폴더 전체 크기를 묶지만, 덤프 하나가 0.5~0.9GB라 5GB 안에 6~9개만 남습니다.
- 보관 덤프 31건(크래시 19, 프리징 10, 수동 2, 1.6.1170, 모듈 약 480~520개)을 PE 섹션 단위로 쟀습니다.
  - 프리징·수동 덤프(약 560MB)는 거의 전부 모듈 코드(`.text`)입니다. SKSE 플러그인 약 235MB, DriverStore 그래픽 드라이버 약 120MB, Windows 약 83MB, 그 밖의 모드 DLL 약 56MB(Prisma UI의 `WebCore.dll` 34MB 등), `SkyrimSE.exe` 23.3MB입니다. 출처는 `MiniDumpWithCodeSegs`이고, v0.2.44에서 외부 역공학 도구가 덤프에서 기계어를 찾지 못하던 문제 때문에 `DumpMode=1` 기본값에 들어갔습니다.
  - 크래시 덤프(730~875MB)는 여기에 모든 모듈의 리소스(`.rsrc`) 약 260MB, `.pdata` 약 24MB(ADR-0011이 담는 Windows 모듈 몫 약 8MB 포함), 재배치(`.reloc`) 약 5MB가 더해집니다. 출처는 크래시 프로필에만 있는 `MiniDumpWithModuleHeaders`입니다. 이 플래그는 헤더만이 아니라 데이터 디렉터리의 내용까지 담습니다. 작은 프로세스로 플래그 조합별 덤프를 써서 확인했습니다(이 플래그만 켜도 `.rsrc`·`.reloc`·`.pdata`가 담김). 이 PC에서는 DLSS 모델이 리소스로 든 `nvngx_dlssnr.dll` 하나가 250MB였습니다.
  - 모듈 밖 메모리(스택, 오브젝트, TEB)는 0.5~10MB입니다.
- 분석기가 덤프의 코드 바이트를 읽는 곳은 모두 어떤 스레드의 RIP나 스택에 있는 값(리턴 주소) 근처입니다.
  - 예외 명령어(ADR-0019): 예외 주소의 최대 15바이트, `[RSP]` 리턴 주소 앞 15바이트(호출 위치)
  - 설치 중인 후킹(ADR-0016): 예외 주소의 점프 코드, `[RSP]` 리턴 주소 앞의 `call`
  - 메인 스레드 대기 분류(ADR-0009): 메인 스레드 위쪽 12프레임의 리턴 주소 앞 0x60바이트
  - 엔진 스핀 루프: 메인 스레드 RIP 앞뒤 32바이트
  - Crash Logger 스레드 판별: 스택 값 앞 7바이트, 소유 스레드 RIP 앞 2바이트
  - 스택 해제(`StackWalk64`): 프레임 IP가 에필로그 안인지 볼 때 그 함수의 코드(DbgHelp 내부 동작이라 추정)

  앞의 셋은 덤프 자신의 메모리만 읽습니다. 디스크의 `SkyrimSE.exe`는 코드가 암호화돼 있기 때문입니다(Steam DRM `.bind` 섹션이 있고, `.text`를 64바이트씩 373곳 비교하면 덤프와 디스크가 한 곳도 같지 않음). 나머지는 덤프에 없으면 같은 빌드의 로컬 파일에서 읽습니다. 스택이 가리키지 않는 코드를 읽는 분석은 없습니다.
- 5건을 재 보니, 모든 스레드(245~291개)의 RSP부터 스택 끝까지의 값 중 실행 섹션을 가리키는 것은 덤프마다 345~2,823개였습니다. 그 값을 담은 `.pdata` 함수를 모두 합쳐도 0.1~0.3MB였습니다.
- `MINIDUMP_CALLBACK`으로 모듈별 선택이 됩니다. `ModuleCallback`에서 `ModuleWriteCodeSegs`를 지우면 그 모듈의 코드만 빠집니다(작은 프로세스에서 24.7MB → 5.9MB). 콜백 순서는 IncludeThread → IncludeModule → Module → Thread → Memory입니다. 그래서 모듈 선택은 스레드 컨텍스트를 보기 전에 정해야 하고, 스택에서 찾은 범위는 ADR-0010 부록(프리징 오브젝트)처럼 첫 `MemoryCallback`에서 더해야 합니다.

## Decision (제안)
1. **코드는 게임 EXE만 통째로**: `DumpMode=1`의 크래시·프리징·수동 덤프는 게임 EXE(프로세스의 주 이미지)만 코드 전체를 담습니다. 다른 모듈은 `ModuleCallback`에서 `ModuleWriteCodeSegs`를 끕니다.
   - 게임 EXE를 남기는 이유: 디스크의 코드가 암호화돼 덤프 말고는 얻을 곳이 없고, SKSE 플러그인이 메모리에서 고친 후킹 지점도 덤프에만 남습니다. 1.6.1170에서 23.3MB입니다.
   - 크래시 재수집(`_Richer`, `_Full`)과 `DumpMode=2`는 지금처럼 모든 모듈 코드를 담습니다. 재수집은 더 많이 담으려고 켜는 기능입니다.
   - PSS 스냅샷 경로(`EnablePssSnapshotForFreeze`, 기본 꺼짐)는 아래 3번 범위를 더할 수 없어(ADR-0011과 같은 이유) 지금처럼 모든 모듈 코드를 담습니다.
2. **`MiniDumpWithModuleHeaders` 대신 필요한 디렉터리만**: 크래시 기본 프로필과 크래시 재수집에서 이 플래그를 뺍니다. 대신 ADR-0011 수집을 넓힙니다.
   - 대상: Windows 폴더 모듈(지금처럼) + 아래 3번에서 스택이 가리킨 모든 모듈(게임 EXE, SKSE 플러그인, 그 밖의 DLL)
   - 내용: PE 헤더, export 디렉터리, `.pdata`, UNWIND_INFO(ADR-0011과 같은 계산). 리소스, 재배치, import 디렉터리는 담지 않습니다.
   - 오래된 DLL은 UNWIND_INFO를 `.text` 안에 둡니다(예: DirectX 2010의 `XAudio2_7.dll`). 지금은 코드 영역에 함께 담겨 왔지만, 코드를 끄면 이 수집만이 출처입니다. ADR-0011 수집은 섹션과 상관없이 RVA로 읽으므로 그대로 쓸 수 있습니다.
   - 크래시 덤프는 스택에 없는 모듈의 `.pdata`를 잃지만, 스택 해제는 스택에 있는 프레임만 풉니다.
   - 스택에 있는 모드 DLL은 UNWIND_INFO까지 담깁니다. 그래서 모드를 업데이트한 뒤에도 그 프레임을 지나 스택을 풉니다. 지금은 크래시 덤프에 `.pdata`만 있고 UNWIND_INFO가 없어 풀지 못합니다(ADR-0011의 "모드 DLL은 대상이 아니다"가 이 모듈들에서 풀림).
   - 상한: Windows 모듈의 24MB와 따로, 스택 모듈 16MB(보관 덤프에서 새로 더해질 양은 2.8~10.6MB)
3. **스택이 가리키는 코드**: 첫 `MemoryCallback`에서(모든 스레드가 보고되고 멈춰 있을 때) 스레드마다 다음을 더합니다.
   - RIP 앞뒤 0x100바이트. 모듈 밖이어도 읽을 수 있으면 담습니다(후킹 트램펄린, ADR-0016).
   - 크래시 덤프는 예외 컨텍스트의 RIP도 같은 범위로 담습니다. 크래시 스레드의 현재 컨텍스트는 크래시 핸들러 안이기 때문입니다. 핸들러는 같은 스택의 더 아래에서 돌므로, 현재 RSP부터 훑으면 예외 시점의 스택도 들어갑니다.
   - RSP부터 스택 끝까지의 8바이트 값 중 모듈의 실행 섹션을 가리키는 것마다, 그 값을 담은 `.pdata` 함수(64KB 이하; 리턴 주소는 값−1로 찾음)와 값 −0x100~+0x40을 담습니다. 대기 분류의 0x60바이트는 함수 시작보다 앞을 읽을 수 있어서 창이 따로 필요합니다.
   - 상한 8MB(측정 최대 0.3MB)
4. **예전 방식 선택지**: ini `DumpAllModuleCode=0`(기본)을 둡니다. 1이면 v0.2.71까지처럼 모든 모듈의 코드를 담습니다. 외부 역공학 도구로 덤프만 보는 사람을 위한 것입니다.
5. **기록**: 매니페스트의 `include_code_segments`는 그대로 두고 `code_segments_scope`(`all` 또는 `game_and_stacks`)를 더합니다. 리포트의 `CaptureProfileCodeSegments` 줄에도 같은 값을 붙입니다.
6. `MaxTotalDumpGB` 기본값(5GB)은 그대로 둡니다.

## Alternatives Considered
- **게임 EXE 코드도 빼기**: 덤프마다 23MB가 더 줄지만, 디스크에서 다시 얻을 수 없는 유일한 코드이고 메모리 후킹 흔적도 함께 잃습니다.
- **모든 모듈의 UNWIND_INFO 담기**: 덤프마다 약 29~48MB(추정)입니다. 스택 해제는 스택에 있는 프레임만 보므로, 스택이 가리킨 모듈이면 충분합니다.
- **크기 기준으로 큰 모듈만 빼기**: 무엇이 빠질지가 모드 구성에 따라 달라지고, 작은 모드 DLL 수백 개의 합(약 235MB)은 줄지 않습니다.
- **덤프 압축**: 뷰어와 DbgHelp는 압축하지 않은 덤프를 열어야 하고, 캡처 때 CPU 시간이 더 듭니다.
- **개수 기본값만 낮추기**: 사건 하나의 크기는 그대로라 기록을 덜 남길 뿐입니다.

## Consequences

### Positive
- 덤프 크기(아래 시뮬레이션으로 추정): 크래시는 728~875MB에서 41~83MB로, 프리징·수동은 382~571MB에서 32~58MB로 줄어듭니다. 지금 모드 구성(10월 18일 이후 덤프)에서는 크래시 67~83MB, 프리징·수동 56~58MB입니다. 보관 덤프 31건 합계는 21.7GB에서 약 1.8GB가 됩니다. 5GB 상한에 크래시 덤프 60개 이상이 들어가므로, 기본 개수 상한(종류별 20개)이 먼저 걸립니다.
- 쓸 바이트가 10분의 1 안팎이라, 덤프를 쓰는 동안 게임이 멈춰 있는 시간도 줄어들 것입니다(미측정).
- 스택에 있는 모드 DLL 프레임은 모드를 업데이트한 뒤에도 덤프만으로 풀립니다.

### Limitations
- 스택에 없는 함수는 게임 EXE 밖이면 덤프만으로 디스어셈블할 수 없습니다. 같은 빌드의 파일이나 Microsoft 심볼 서버의 이미지가 있어야 합니다. 필요하면 `DumpAllModuleCode=1`을 씁니다.
- 스택은 RSP 위쪽만 봅니다. 이미 반환한 함수(RSP 아래에 남은 값)는 담지 않습니다. 분석기도 그 영역을 보지 않습니다.
- 헬퍼가 덤프를 쓸 때 모든 스레드의 스택(합계 약 0.5MB)을 한 번 더 읽고, 스택 모듈의 `.pdata`를 읽는 시간이 듭니다.
- 이 버전 이전의 덤프는 그대로입니다.

## Verification
- **시뮬레이션(2026-10-30, 결정 전)**: 보관 덤프 31건마다 제안 범위 밖의 모듈 메모리(게임 EXE 밖의 코드, `.rsrc`, `.reloc`, 스택에 없는 비 Windows 모듈의 `.pdata`)를 지운 사본을 만들었습니다. 원본과 사본을 같은 분석기(로컬 v0.2.71 빌드, main `abf5981`까지 분석기 코드 변경 없음)로 분석해, 요약의 주요 필드(`scripts/compare_analyses.py`의 비교와 같음)를 비교했습니다.
  - 로컬 파일 사용(평소 분석): 31건 모두 같음
  - `SKYDIAG_STACKWALK_IGNORE_LOCAL_IMAGES=1`(다른 PC나 업데이트 뒤): 31건 모두 같음
  - 사본은 원본에 이미 있던 바이트만 남깁니다. 2번이 새로 더할 모드 DLL의 UNWIND_INFO는 원본에 없어서, 덤프만 쓰는 비교는 실제 구현보다 불리한 조건입니다.
  - 섹션 위치와 `.pdata`는 같은 빌드의 로컬 파일에서 읽었습니다. 그 뒤 업데이트된 모듈은 지우지 않고 원본대로 두었습니다. 그래서 9월 18일~10월 13일 덤프 14건은 모듈 82~138개가 그대로였고, 10월 18일 이후 17건은 3~14개만 그대로였습니다. 위 크기 추정은 이 모듈들이 남긴 바이트(1~435MB)를 뺀 값입니다. 실제 헬퍼는 실행 중인 게임 메모리를 읽으므로 이 제약이 없습니다.
  - 첫 실행에서는 덤프만 쓰는 비교에서 프리징 1건이 달라졌습니다(`XAudio2_7.dll` 프레임 다음에서 스택 해제가 멈춤). 원인은 이 DLL의 UNWIND_INFO가 `.text` 안에 있어 시뮬레이션이 코드와 함께 지운 것이었습니다. 2번 수집이 담는 범위를 시뮬레이션에도 남기자 같아졌습니다. 2번 수집이 이런 DLL에서 UNWIND_INFO의 유일한 출처라는 근거입니다.
- 구현할 때
  - 헬퍼 자기 덤프 테스트: 주 이미지 밖 모듈의 코드는 스택이 가리킨 범위만, 주 이미지 코드는 전부 담기는지
  - `skydiag_modal_dialog_hang_e2e_dump_only_tests`가 새 범위로도 MessageBox 대기 체인을 찾는지
  - 보관 덤프 비교(`compare_analyses.py`)와 실게임 수동 캡처 한 건의 크기·분석 비교(사용자 확인)

## References
- `helper/src/DumpWriter.cpp`, `helper/src/DumpProfile.cpp`, `helper/src/ModuleUnwindMemory.cpp`, `helper/src/HangObjectMemory.cpp`
- `dump_tool/src/AnalyzerInternalsFaultInstruction.cpp`, `dump_tool/src/AnalyzerInternalsHookStubRace.cpp`, `dump_tool/src/AnalyzerInternalsStackwalk.cpp`, `dump_tool/src/AnalyzerInternalsStackwalkMemory.cpp`
- `docs/adr/0009-main-thread-wait-and-game-state.md`, `docs/adr/0010-crash-object-introspection.md`, `docs/adr/0011-dump-carries-system-module-unwind-data.md`, `docs/adr/0016-hook-install-race.md`, `docs/adr/0019-faulting-instruction.md`
- Microsoft Learn: `MINIDUMP_TYPE`, `MODULE_WRITE_FLAGS`, `MINIDUMP_CALLBACK_TYPE`
