# Changelog

> **버전 갭 안내:** v0.2.7, v0.2.24, v0.2.38은 RC(Release Candidate)만 배포 후 정식 릴리즈 없이 다음 버전으로 넘어간 번호입니다.

## v0.2.60-rc4 (2026-10-09)

### 한눈에 보기
- rc3를 실게임에 설치해 수동 캡처가 평소처럼 동작하는 것을 확인했습니다(576MB 덤프, SKSE 로그 매칭, 리포트 정상, 정상 종료).
- 실사고 덤프 11개 재분석과 코드 점검에서 찾은 문제 중 작고 효과가 분명한 것을 고쳤습니다. 플러그인 이름 때문에 Helper가 종료돼 프리징 덤프를 잃던 문제, 다시 실행하면 지워지던 Helper 로그, 매번 뜨던 권한 경고, 그리고 조치가 아닌 문장이 "다음 조치"로 나오던 문제입니다.

### 수정
- **플러그인 이름 때문에 Helper가 종료되던 문제** — `plugins.txt`, `Skyrim.ccc`, 플러그인 헤더의 마스터 목록은 UTF-8이 아니라 시스템 ANSI 코드 페이지로 기록됩니다. `é` 같은 문자가 든 플러그인 이름이 있으면 플러그인 스캔에서 예외가 나고, 이를 받는 곳이 없어 Helper가 종료됐습니다. 프리징 캡처는 스캔 뒤에 덤프를 쓰므로 그 덤프도 남지 않았습니다.
  - 이 이름들을 UTF-8로 바꿔 다루고, 그래도 남는 잘못된 문자는 대체 문자로 기록합니다.
  - 플러그인 스캔이 어떤 이유로 실패해도 Helper 로그에 남기고, 스캔 없이 캡처를 계속합니다.
- **Helper 로그 보존** — Helper가 게임에 붙을 때마다 `SkyrimDiagHelper.log`를 비웠습니다. 크래시 뒤 게임을 다시 켜면 확인해야 할 로그가 사라졌습니다. 이제 이전 실행의 로그를 `SkyrimDiagHelper.previous.log`로 남깁니다.
- **권한 경고 정리** — 관리자가 아닌 사용자는 캡처할 때마다 `Warning: EnableDebugPrivilege failed; WCT capture may be incomplete.`가 찍혔습니다. 이 경우(`ERROR_NOT_ALL_ASSIGNED`)에도 Helper는 게임과 같은 사용자로 돌기 때문에 게임 스레드의 대기 정보는 그대로 읽습니다. 이제 이 경우에는 경고를 남기지 않고, 다른 오류일 때만 오류 코드와 함께 남깁니다.
- **다음 조치 선택** — 리포트의 `NextAction`은 체크리스트 첫 줄이었습니다. 실사고 CTD 5건 중 2건은 "ExceptionCode=0xC0000005(접근 위반)입니다…"라는 설명, 3건은 "유력 후보 모드를 업데이트/재설치…"라는 일반 문구가 나왔고, 실제 후보는 그 아래 있었습니다.
  - 이제 modal 대화상자, 행동 우선 후보, Crash Logger 프레임, 오브젝트 참조, 충돌 순으로 고르고, 없으면 배경 설명이 아닌 첫 줄을 씁니다.
  - 요약 JSON에 `next_action_index`를 추가했고, WinUI도 같은 줄을 씁니다.
  - 다시 분석한 5건 모두 실제 후보(SkyrimUpscaler, CalamityAffixes, DragonWar, SmoothCam, `AE_StellarBlade_Doro.esp`)를 가리킵니다.
- **보관 정리 방어** — 출력 폴더의 파일 이름을 시스템 코드 페이지를 거치지 않고 비교하고, 정리 작업이 실패해도 Helper를 멈추지 않고 로그에 남깁니다.

### 주의사항
- Helper와 분석기, WinUI가 바뀌었습니다. zip 전체를 교체해 주세요.
- 프리징 리포트의 결론은 아직 약합니다. 실사고 프리징 5건이 모두 근거가 약하거나 맞지 않았고, 다음 버전에서 다룰 예정입니다.

### 테스트
- Windows 전체 테스트: `79/79` 통과.
- ANSI 플러그인 이름 스캔(수정 전 코드에서는 실패), 코드 페이지 밖 이름이 섞인 폴더 정리, 이전 Helper 로그 보존, 실사고 체크리스트로 만든 다음 조치 선택을 테스트로 고정했습니다.

## v0.2.60-rc3 (2026-10-09)

### 한눈에 보기
- rc2의 SKSE 미로드 DLL 목록 묶기를 실게임에서 확인했습니다. `[SKSE]` 줄과 근거 항목에 `msdia140.dll, NpcGhostFix.dll: 버전 정보 없음(…)`이 한 번만 나왔습니다.
- CI에서만 간헐적으로 실패하던 덤프 테스트의 원인을 찾아 고쳤습니다. AMX를 지원하는 Intel 서버 CPU와 Windows Server 2022의 dbghelp 조합에서 CTD 덤프 쓰기가 실패하던 문제로, 같은 환경의 사용자 PC에서도 CTD 덤프를 놓칠 수 있었습니다.

### 수정
- **AMX CPU에서 CTD 덤프 쓰기 실패** — dbghelp 10.0.20348(Windows Server 2022)은 예외 컨텍스트를 CPU 확장 상태 전체 크기만큼 읽어 덤프에 씁니다. AMX가 있는 CPU(Intel Xeon Sapphire/Emerald/Granite Rapids 등)에서는 이 크기가 약 11.5KB인데, Helper가 넘기던 것은 1,232바이트짜리 `CONTEXT`였습니다. 그 뒤가 읽을 수 없는 메모리면 덤프 파일 쓰기가 `0x800706F8`(ERROR_INVALID_USER_BUFFER)로 실패하고, 재시도해도 같았습니다.
  - 이제 예외 컨텍스트를 `InitializeContext(CONTEXT_ALL | CONTEXT_XSTATE)`로 만든 확장 상태 크기 버퍼에 담아 넘깁니다. 레지스터 값은 그대로이고, 분석기는 컨텍스트의 앞부분만 읽으므로 리포트에는 변화가 없습니다.
  - 덤프가 실패하면 Helper 로그에 오류를 16진수로도 적고, 실제로 로드된 dbghelp.dll의 버전과 경로를 함께 남깁니다.
  - 일반 게이밍 PC(AMD, Intel 데스크톱 CPU)는 AMX가 없어 이전에도 해당하지 않았을 가능성이 큽니다.

### 정정
- v0.2.59와 그 rc5·rc6 노트는 이 간헐적 실패를 `ERROR_PARTIAL_COPY`로 설명했지만 잘못 읽은 것이었습니다. 로그의 `2147944184`는 `0x800706F8`(ERROR_INVALID_USER_BUFFER)이고, 기록된 실패 5건이 모두 이 코드였습니다. 그래서 rc5에 넣은 "읽을 수 없는 메모리를 건너뛰는 재시도"는 이 실패에서 한 번도 동작하지 않았습니다(ERROR_PARTIAL_COPY 대비로는 남겨 둡니다).

### CI
- Windows 테스트 전에 러너의 CPU, OS 빌드, AVX-512 지원, dbghelp 버전을 기록합니다.
- dump-stress 워크플로를 16개 job으로 나눠 여러 호스트에서 돌리고, `SKYDIAG_DUMP_IO_TRACE=1`로 덤프 파일 쓰기를 추적합니다. 이 변수는 테스트에서만 쓰며 게임 환경의 동작은 바꾸지 않습니다.

### 주의사항
- Helper가 바뀌었습니다. zip 전체를 교체해 주세요.

### 테스트
- Windows 전체 테스트: `78/78` 통과.
- 수정 전 dump-stress(16개 job)에서 AMX 호스트 2대(Xeon 6973P-C, Xeon Platinum 8573C)만 실패하고, AVX-512만 있는 Intel·AMD 호스트 14대는 통과했습니다. 수정 후 3회 실행(48개 job)이 AMX 호스트 4대를 포함해 모두 통과했습니다.
- 덤프 쓰기 추적 경로와, 덤프 안 예외 컨텍스트의 RIP·RSP가 원래 값과 같은지를 테스트로 확인합니다.

## v0.2.60-rc2 (2026-10-09)

### 한눈에 보기
- rc1의 skse64.log 기능을 실게임에서 확인했습니다. 수동 캡처 한 번에 Helper 로그에 `SKSE log: matched (checked=322, loaded=320, not_loaded=2)`가 남았고, 리포트에 `SkseLog:` 줄, `[낮음]` 근거 항목, 권장 조치 끝부분의 `[SKSE]` 줄이 나왔습니다. 다음 조치는 바뀌지 않았습니다.
- 같은 확인에서 거부된 DLL 두 개의 사유가 같아, 긴 설명이 DLL마다 반복되는 것을 고쳤습니다.

### 수정
- **SKSE가 로드하지 않은 DLL 목록 묶기** — 사유가 같은 DLL은 한 번에 적습니다. 예: `msdia140.dll, NpcGhostFix.dll: 버전 정보 없음(…)`. 처음 나온 순서를 지키고, 표시 개수 제한(6개)은 DLL 수로 셉니다. 권장 조치에서는 사유 묶음 사이를 `; `로 나눕니다.

### 주의사항
- 분석기만 바뀌었습니다. zip 전체를 교체해 주세요.

### 테스트
- Windows 전체 테스트: `78/78` 통과.
- 같은 사유 묶기, 다른 오류 코드는 따로 두기, 개수 제한을 DLL 수로 세기를 테스트로 고정했습니다.
- rc1 실게임 수동 캡처를 다시 분석해 한/영 리포트 모두 설명이 한 번만 나오는 것을 확인했습니다.

## v0.2.60-rc1 (2026-10-09)

### 한눈에 보기
- 플러그인이 띄운 Address Library 오류 창 때문에 게임이 멈추면, 리포트가 어떤 플러그인을 어떻게 바꿔야 하는지 안내합니다. v0.2.59 실게임 확인 때 SmoothCam.dll이 "Unsupported address library format: 2" 창으로 게임을 멈췄는데, 그때 리포트는 창 내용을 보여주고 "메시지를 따르세요"라고만 했습니다.
- 그 실행에서 SKSE가 로드하지 않은 DLL과 사유를 리포트에 참고로 보여줍니다. 원인 후보로는 쓰지 않습니다.

### 추가
- **Address Library 오류 창 안내** — 프리징이 modal 대화상자 때문이고 본문이 CommonLibSSE(-NG)의 Address Library 오류 문구이면 종류를 붙입니다(ADR-0006).
  - `plugin_incompatible`("Unsupported address library format", "Failed to find the id within the address library"): 플러그인 빌드가 현재 게임 버전을 지원하지 않으므로, 이 게임 버전용 파일로 바꾸거나 모드를 비활성화하라고 안내합니다. 보통 Address Library를 다시 설치해서는 해결되지 않습니다.
  - `address_library_missing`("Failed to locate an appropriate address library", "failed to open address library file"): 이 게임 버전용 Address Library를 설치하라고 안내하고, 이미 있다면 플러그인이 다른 판(SE/AE)용일 수 있다고 덧붙입니다.
  - 요약 문장에 한 줄을 덧붙이고 일반 안내("메시지를 따르세요")를 플러그인 이름과 게임 버전을 짚는 안내로 바꿉니다. 호출 모듈을 못 찾으면 CommonLib이 창 제목에 넣는 플러그인 파일 이름을 씁니다. 신뢰도와 후보는 바뀌지 않습니다.
  - 리포트의 `modal_dialog_wait` 줄, 근거 항목, 요약 JSON의 `freeze_analysis.modal_dialog_wait.address_library_issue`에 기록합니다.
- **SKSE가 로드하지 않은 DLL** — SKSE는 게임 버전과 맞지 않는 플러그인을 시작할 때 거부하고 사유를 skse64.log에 남깁니다. 거부된 DLL은 실행되지 않으므로 사고 원인이 될 수 없어, 참고 정보로만 보여줍니다(ADR-0008).
  - Helper가 캡처할 때 게임 EXE의 기준 주소를 함께 읽고, `Documents\My Games\Skyrim Special Edition*\SKSE\skse64.log` 중 첫머리 `imagebase`가 같은 로그만 이번 실행의 로그로 씁니다. skse64.log에는 시각 정보가 없어 이전 실행의 로그를 이렇게 걸러냅니다. 여러 개가 맞으면 가장 최근 로그를 씁니다.
  - 결과는 플러그인 스캔의 `skse_log`에 실리고, Helper 로그에 `SKSE log: matched (checked=..., loaded=..., not_loaded=...)` 한 줄이 남습니다.
  - 리포트에는 `Low` 근거 항목, 체크리스트 맨 끝의 `[SKSE]` 안내, `SkseLog:` 줄로 나옵니다. 안내에는 이번 사고의 원인 근거가 아니라고 적습니다.
  - SKSE는 DLL 이름을 시스템 ANSI 코드 페이지로 기록하므로, UTF-8로 바꿔서 스캔 JSON에 넣습니다.

### CI
- Windows 빌드가 새로 만든 테스트 exe를 링크하다 "Access is denied" / "used by another process"로 실패하면 한 번 다시 빌드합니다(`ci.yml`, `release.yml`).
- skse64.log 파서용 퍼저 `fuzz_skse_log_parser`를 Linux fuzz 스모크에 추가했습니다.

### 주의사항
- 분석기와 Helper가 바뀌었습니다. 플러그인 코드와 공유 메모리 프로토콜(SharedLayout v4)은 v0.2.59와 같지만 버전 정보가 바뀌므로 zip 전체를 교체해 주세요.
- `msdia140.dll`처럼 SKSE 플러그인이 아닌 보조 DLL도 SKSE가 "no version data"로 기록하므로 목록에 나옵니다. 구형 플러그인과 구분할 수 없어 안내에 두 가능성을 함께 적었습니다.
- 이 버전 이전의 캡처에는 SKSE 로그 정보가 없습니다.

### 테스트
- Windows 전체 테스트: `78/78` 통과.
- 문구 분류(실제 SmoothCam 본문 포함), 오류 종류별 한/영 요약과 안내, skse64.log 파서(실제 2.2.6 로그 형식, 공백과 괄호가 든 이름, 로드 후 충돌, 오류 코드, 깨진 줄), 스캔 JSON 파싱, 기준 주소 매칭과 다른 실행의 로그 거부, ANSI 이름의 UTF-8 직렬화, `[SKSE]` 안내의 조건과 위치를 테스트로 고정했습니다.
- 보관 중인 SmoothCam 실사고 프리징 덤프를 다시 분석해 `address_library_issue=plugin_incompatible`과 새 안내를 확인했습니다.
- 사용자 환경의 실제 skse64.log(1.6.1170, SKSE 2.2.6)를 파싱한 결과(검사 322, 로드 320, 거부 2)가 로그를 직접 센 값과 같습니다. 실게임 캡처에서 Helper가 로그를 매칭하는지는 rc1 실게임 테스트로 확인해야 합니다.

## v0.2.59 (2026-10-09)

v0.2.59-rc1부터 rc7까지의 변경을 묶은 정식 릴리즈입니다. 항목별 자세한 내용은 아래 각 RC 항목에 있습니다.

### 한눈에 보기
- **원인 지목을 더 보수적으로, 근거에 맞게** — 같은 사고의 실행 위치를 가리키는 두 신호를 독립 교차검증으로 세지 않고, 보강 신호만으로 `High`가 되지 않습니다(rc1). 게임 본체(EXE) 크래시에서 버전 불일치나 후킹 충돌을 단정하지 않습니다(rc6).
- **프리징 분석** — 메인 스레드 기준 귀속과 스레드 그룹 합의(rc1), 플러그인 오류 대화상자 때문에 멈춘 경우를 가리는 `modal_dialog_wait`(rc2)를 추가했습니다.
- **게임 플레이 중 프리징 판정 기준** — 메뉴가 한 번 열리면 "메뉴 중" 상태가 끝까지 꺼지지 않아, 게임 플레이 중에도 메뉴 기준(30초)으로 판정하던 첫 버전부터의 버그를 고쳤습니다. 이제 설정한 `HangThresholdInGameSec`(기본 10초)가 적용됩니다(rc7).
- **실제 호출 체인 복원** — 정식 stackwalk가 1프레임에서 멈추던 버그를 고쳤고(rc2), 코드가 아닌 주소가 프레임으로 섞이지 않으며(rc3), PDB 없는 DLL을 먼 export 이름으로 부르지 않습니다(rc4).
- **Crash Logger와 함께 쓸 때** — Crash Logger가 보고 중인 원래 예외(assert 등)를 기록하고(rc3), Crash Logger 자신의 메모리 조사 예외는 크래시로 기록하지 않습니다(rc5). 예외 위치가 Crash Logger면 2차 예외로 해석하도록 안내합니다(rc3).
- **플러그인 스캔** — 크래시 후 스캔이 항상 건너뛰어지던 문제(rc3)와, MO2 + Anniversary Edition에서 Creation Club 파일이 모두 누락 마스터로 잘못 판정되던 문제(rc4)를 고쳤습니다.
- **Helper** — 게임 플레이 중에도 수동 캡처 단축키를 잡고(rc6), 크래시 없는 비정상 종료를 "캡처 실패"로 적지 않으며(rc5), 덤프가 실패하면 실패 위치를 남깁니다(rc6). Linux(Wine/Proton)에서는 모든 캡처에 텍스트 리포트를 만듭니다(rc2).
- **리포트** — hang 리포트도 Windows 표시 언어를 따르고(rc5), 신뢰도 낮은 플러그인 규칙이 다음 조치를 차지하지 않으며(rc4), 브레이크포인트 예외를 설명합니다(rc3).

### 실게임에서 확인한 것
- rc2~rc7을 한 사용자의 MO2 환경(게임 1.6.1170, 플러그인 약 4,250개)에서 실제로 플레이하며 확인했습니다.
- 확인됨: `modal_dialog_wait`(SmoothCam 오류 창, 신뢰도 High), 크래시 후 플러그인 스캔, Creation Club 누락 마스터 0개, Crash Logger 스레드 덤프 중 크래시 이벤트 없음, 실사고 덤프 재분석에서의 리포트 언어와 게임 본체 크래시 안내, 게임 플레이 중 수동 캡처 단축키(키 한 번에 캡처 한 번), 프리징 판정 기준(게임 플레이 중 10초·인벤토리 30초, 세이브 불러오기·저장·종료 때 프리징 캡처 없음).
- 실게임 미확인: Crash Logger가 보고한 assert의 원래 예외 기록(같은 순서를 재현하는 런타임 테스트로만 확인), 덤프 실패 위치 진단(실패가 재현되지 않음).

### 정정
- rc5 노트의 "`ERROR_PARTIAL_COPY`로 실패하면 읽을 수 없는 메모리를 건너뛰고 다시 써서 CTD 덤프를 놓치지 않도록 했다"는 설명은 맞지 않았습니다. 이 재시도가 들어간 뒤에도 CI에서 같은 실패가 났습니다. 원인은 아직 모르며, 로컬 100회와 CI 러너 단독 400회 반복에서는 재현되지 않았고 전체 CI 실행 안에서만 간헐적으로 납니다. rc6부터 덤프가 실패하면 실패 위치를 Helper 로그에 남깁니다.

### 주의사항
- 플러그인, Helper, 분석기, WinUI 런처가 모두 바뀌었습니다. 기존 파일 일부만 덮지 말고 zip 전체를 교체해 주세요. 공유 메모리 프로토콜(SharedLayout v4)은 v0.2.58과 같습니다.
- 게임 플레이 중 프리징 판정 기준이 실제로 30초에서 설정값(기본 10초)으로 바뀝니다. 정상적인 긴 멈춤이 프리징으로 기록되면 `SkyrimDiagHelper.ini`의 `HangThresholdInGameSec`로 조정할 수 있습니다. 이전 버전 캡처의 `in_menu: true`는 실제 메뉴 상태와 다를 수 있습니다.
- crash bucket이 v3(`CTD3-`)로 바뀌어 기존 `CTD2-` 이력과 자동으로 합쳐지지 않습니다.
- stackwalk는 덤프의 모듈과 같은 버전의 파일이 분석하는 PC에 있어야 그 모듈을 지나 내려갑니다.
- Crash Logger 1.25도 `Ctrl+Shift+F12`를 스레드 덤프 단축키로 써서, 둘 다 설치되어 있으면 한 번 누를 때 두 결과가 함께 생깁니다.
- Crash Logger가 SkyrimDiag가 기록하거나 보관하지 않는 드문 예외를 보고하면 SkyrimDiag 덤프가 남지 않습니다(Crash Logger 로그는 남음).
- 검수된 실사고 코퍼스가 없어 다른 크래시 로거 대비 적중률을 수치로 주장하지 않습니다.

## v0.2.59-rc7 (2026-10-08)

### 한눈에 보기
- rc6의 게임 플레이 중 수동 캡처 단축키 수정을 실게임에서 확인했습니다. 메뉴를 닫고 누른 `Ctrl+Shift+F12`가 새 키 상태 감시로 잡혀(`via key-state poll`) 캡처가 한 번 만들어졌고, 크래시 이벤트는 없었습니다.
- 같은 확인에서 메뉴를 모두 닫았는데도 Helper 로그에 `inMenu=1`이 찍혀, 오래된 버그를 찾았습니다. 게임을 시작하며 메뉴가 한 번 열리면 "메뉴 중" 상태가 끝까지 꺼지지 않았습니다. 그래서 게임 플레이 중 프리징 판정 기준이 설정한 `HangThresholdInGameSec`(기본 10초)가 아니라 메뉴 기준 `HangThresholdInMenuSec`(기본 30초)였습니다.

### 수정
- **"메뉴 중" 상태가 꺼지지 않던 문제** — 플러그인은 메뉴가 열릴 때마다 "메뉴 중" 상태를 켜고, 메뉴가 닫힐 때 `UI::IsShowingMenus()`가 거짓이면 껐습니다. 그런데 이 함수는 "열린 메뉴가 없음"이 아니라 "HUD를 보여 주고 있음"(콘솔 `tm`으로 끄는 표시)을 뜻해서, 평소 플레이 중에는 항상 참이었습니다. TrueHUD, 커서 메뉴, 위젯 메뉴처럼 플레이 내내 열려 있는 HUD형 메뉴도 열릴 때 상태를 켰습니다.
  - 이제 게임을 멈추는 메뉴(인벤토리·지도·저널·콘솔 등), 모달 창, 메인 메뉴 같은 앱 메뉴, 아이템 메뉴가 실제로 열려 있을 때만 "메뉴 중"으로 봅니다. 게임이 관리하는 메뉴 개수로 판단하고, 막 열리는 메뉴는 그 메뉴 자신의 성격으로도 판단합니다.
  - 메뉴가 열리고 닫힐 때뿐 아니라 100ms마다 도는 Heartbeat에서도 다시 계산하므로, 메뉴 이벤트 시점에 상태가 어긋나도 다음 Heartbeat에서 바로잡힙니다.
  - 게임이 모든 메뉴를 한꺼번에 닫는 동안(세이브 불러오기, 게임 종료)에는 끄지 않습니다.
  - 첫 공개 버전부터 있던 버그입니다.

### 주의사항
- 게임 플레이 중 프리징 판정 기준이 실제로 30초에서 설정값(기본 10초)으로 바뀝니다. 10~30초 멈췄다가 풀리는 프리징이 이제 잡히는 대신, 큰 세이브처럼 정상적으로 길게 멈추는 순간이 프리징으로 기록될 수 있습니다. 원치 않는 `SkyrimDiag_Hang_*` 캡처가 생기면 알려 주세요. 기준은 `SkyrimDiagHelper.ini`의 `HangThresholdInGameSec`로 바꿀 수 있습니다.
- 이전에는 `SkyrimDiag.ini`의 `LogMenus=0`이면 "메뉴 중" 상태가 아예 켜지지 않았습니다. 이제 Heartbeat가 계산하므로 메뉴 기준이 적용됩니다.
- 이전 버전 캡처의 `in_menu: true`와 Helper 로그의 `inMenu=1`은 이 버그 때문에 실제 메뉴 상태와 다를 수 있습니다.
- 이번 변경은 플러그인(`SkyrimDiag.dll`)입니다. zip 전체를 교체해 주세요.

### 테스트
- Windows 전체 테스트: `75/75` 통과.
- 소스 가드를 추가했습니다. "메뉴 중" 상태를 HUD 표시 여부로 끄지 않는지, 게임의 메뉴 계수 4가지(일시정지·모달·앱 메뉴·아이템 메뉴)로 계산하는지, 모든 메뉴를 닫는 중에는 끄지 않는지, Heartbeat마다 다시 계산하는지 확인합니다.
- 메뉴 상태는 게임 안에서만 바뀌므로 실제 동작은 rc7 실게임 테스트로 확인해야 합니다.

## v0.2.59-rc6 (2026-10-07)

### 한눈에 보기
- rc5 실게임 테스트에서, 게임 플레이 중 누른 `Ctrl+Shift+F12`로 SkyrimDiag 수동 캡처가 만들어지지 않던 문제를 고쳤습니다. 같은 키를 쓰는 Crash Logger의 스레드 덤프는 만들어졌습니다.
- rc5의 Crash Logger 스레드 덤프 오탐 수정은 같은 테스트에서 확인했습니다. 스레드 덤프 동안 `Crash event signaled`가 한 번도 기록되지 않았습니다.
- 크래시 위치가 게임 본체(EXE)일 때 "버전 불일치나 후킹 충돌 가능성이 크다"고 단정하던 안내를 고쳤습니다.
- **rc5 정정:** rc5에서 넣은 `ERROR_PARTIAL_COPY` 재시도는 CI의 같은 실패를 막지 못했습니다. 대신 덤프가 실패하면 어디서 실패했는지 남기는 진단을 넣었습니다.

### 수정
- **게임 플레이 중 수동 캡처 단축키** — Helper는 `RegisterHotKey`의 `WM_HOTKEY`와, 그것이 실패할 때를 위한 `GetAsyncKeyState` 확인으로 단축키를 받았습니다. 게임 플레이 중에는 `WM_HOTKEY`가 오지 않았고(전날 메뉴 화면에서는 왔음), 대비 경로는 "마지막 확인 이후 눌린 적 있음" 표시를 읽었는데, 이 표시는 모든 프로세스가 함께 쓰는 하나의 비트라 같은 키를 확인하는 Crash Logger 1.25가 가져갔습니다. 이제 별도 스레드가 20ms마다 세 키가 "지금 눌려 있는지"를 보고 키 조합이 새로 눌릴 때마다 한 번 알립니다. 이 상태는 다른 프로그램이 읽어도 사라지지 않습니다. `WM_HOTKEY` 경로는 그대로 두고, 캡처 중에 들어온 입력은 버려서 한 번 누르면 캡처도 한 번만 만들어집니다.

- **게임 본체(EXE) 크래시 안내** — 크래시 위치가 게임 본체라는 것은 어디서 멈췄는지일 뿐 원인은 아닙니다. 그런데 권장 조치는 "Address Library/SKSE 버전 불일치 또는 후킹 충돌 가능성이 큽니다"라고 했고, 다른 단서가 없을 때의 결론도 같은 내용을 신뢰도 중간으로 냈습니다. rc5 실게임 CTD(함정이 모드 크리처에 맞은 순간, 콜스택 전체가 바닐라 엔진 코드)처럼 엔진이 처리하던 플러그인 데이터 문제가 그만큼 흔합니다. 이제 권장 조치는 플러그인 데이터(ESP/ESM·메시), 버전 불일치, DLL 훅을 모두 가능성으로 들고, 다른 단서가 없을 때의 결론은 "이 덤프만으로는 가릴 수 없음"을 신뢰도 낮음으로 냅니다.

- **덤프 실패 위치 진단** — 덤프 기록이 실패하면 오류 메시지와 Helper 로그에, 덤프 기록기가 마지막으로 처리하던 단계(콜백 종류와 스레드 ID·모듈 주소)와 읽지 못한 메모리(개수, 첫 주소·크기·상태 코드)를 함께 남깁니다. 메모리 읽기 실패는 기록만 하고 그 영역을 빼고 계속 쓰므로, 그런 실패로는 덤프 전체가 실패하지 않습니다.

### 정정
- rc5의 "덤프가 `ERROR_PARTIAL_COPY`로 실패하면 읽을 수 없는 메모리를 건너뛰고 한 번 더 써서 CTD 덤프를 놓치지 않도록 했다"는 설명은 맞지 않았습니다. rc5 공개 뒤 main CI(`5e37b96`, 이 재시도가 들어간 커밋)에서 helper 스모크 테스트가 같은 오류로 실패했고, Helper의 세 번 시도가 모두 플래그를 켠 재시도까지 실패했습니다. 이 실패는 `MiniDumpIgnoreInaccessibleMemory`가 다루는 일반 메모리 읽기가 아닌 곳에서 나는 것으로 보이며, 원인은 아직 모릅니다. 재시도 자체는 첫 시도를 바꾸지 않으므로 그대로 두었습니다.

### 주의사항
- 이번 변경은 Helper와 분석기입니다. 플러그인은 rc5와 같지만 zip 전체를 교체해 주세요.
- 덤프 중 읽지 못한 메모리 영역은 이제 첫 시도부터 빠진 채로 덤프가 써집니다. 이전에는 이런 경우 덤프 전체가 실패했습니다.
- 게임 플레이 중 `WM_HOTKEY`가 오지 않은 원인(게임의 입력 처리 방식으로 추정)은 확인하지 못했습니다. 키 상태 확인 경로는 그 원인과 관계없이 동작하도록 만든 것이며, 실게임 확인은 rc6에서 필요합니다.

### 테스트
- Windows 전체 테스트: `75/75` 통과.
- 키 조합 감지 테스트(한 번 누르면 한 번, 누르고 있는 동안 반복 없음, 키 순서와 무관, 일부 키만 눌리면 반응 없음)와, 수동 캡처 경로가 키 상태 감시를 쓰고 공유 비트를 읽지 않는지 확인하는 소스 가드를 추가했습니다.
- 게임 본체 크래시의 권장 조치와 결론 문구 테스트를 추가했고, rc5 실사고 덤프(함정 CTD)를 다시 분석해 새 안내가 나오는 것을 확인했습니다.
- 덤프 실패 진단은 실패를 재현할 수 없어 소스 가드로만 고정했습니다. CI에서 실패한 두 테스트(helper 스모크, 덤프 컨텍스트)를 로컬에서 50번씩, 총 100번 반복 실행했지만 한 번도 실패하지 않았습니다.

## v0.2.59-rc5 (2026-10-07)

### 한눈에 보기
- rc4 실게임 수동 캡처에서 드러난 문제를 고쳤습니다. Crash Logger 1.25가 수동 캡처와 같은 `Ctrl+Shift+F12`로 스레드 덤프를 만들면서 스스로 잡는 접근 위반을 내고, 플러그인이 그것을 크래시로 기록해 Helper가 키를 한 번 누를 때마다 전체 크래시 덤프를 여러 번 쓰고 지웠습니다(그동안 게임 정지).
- hang 리포트가 Windows 표시 언어와 상관없이 항상 영어로 나오던 문제와, 플러그인이 오류 창을 띄우고 게임을 끝낸 경우에도 WER 힌트가 "덤프 캡처 실패"라고 적던 문제를 고쳤습니다.
- 덤프가 `ERROR_PARTIAL_COPY`로 실패하면, 같은 방식으로 다시 실패하던 재시도 대신 읽을 수 없는 메모리를 건너뛰고 한 번 더 씁니다.
- rc4의 Creation Club 누락 마스터 수정은 실게임 수동 캡처로 확인했습니다.

### 수정
- **Crash Logger 자신의 예외는 크래시로 기록하지 않음** — Crash Logger 안에서 난 치명 코드 예외는 이제 두 가지로만 처리합니다. 같은 스레드에 최근 보관한 예외(assert 브레이크포인트·C++ 예외)가 있고 아직 기록된 것이 없으면 그 예외를 기록하고(rc3 동작), 그 밖에는 무시합니다. 스레드 덤프 단축키처럼 Crash Logger가 크래시와 무관하게 메모리를 조사할 때 더 이상 덤프를 쓰지 않고, 크래시 기록 기능은 그대로 다음 크래시를 기다립니다.
- **CLI 리포트 기본 언어** — `--lang` 없이 실행되면 CLI가 Windows 표시 언어를 따릅니다(한국어면 한국어, 그 밖은 영어). Helper가 hang 캡처를 헤드리스로 분석할 때 `--lang`을 넘기지 않아 hang 리포트만 항상 영어였습니다. 이제 뷰어가 쓰는 CTD 리포트와 같은 언어로 나옵니다.
- **WER 힌트 문구** — 힌트 파일이 생기는 두 경우를 구분합니다. 크래시를 기록했지만 덤프 쓰기가 모두 실패한 경우와, 기록된 크래시 없이 비정상 종료 코드로 끝난 경우입니다. 뒤의 경우에는 종료 코드를 적고 "캡처 실패가 아님"을 밝힌 뒤, 플러그인 오류 창(호환되지 않는 Address Library·게임 버전 등)이나 `abort()`로 의도적으로 끝났을 가능성을 먼저 안내합니다.

- **부분 복사 실패 시 덤프 재시도** — `MiniDumpWriteDump`는 메모리 영역 하나만 읽지 못해도 `ERROR_PARTIAL_COPY`(HRESULT `0x8007012B`)로 전체가 실패합니다. Helper의 재시도는 같은 덤프 형식을 써서 같은 이유로 다시 실패했고, 그러면 CTD 덤프가 남지 않았습니다. 이제 이 오류로 실패하면 곧바로 `MiniDumpIgnoreInaccessibleMemory`를 더해 한 번 더 씁니다. 첫 시도는 그대로입니다. CI의 덤프 테스트가 이 오류로 간헐 실패해 왔고, #29 병합 커밋에서는 자기 프로세스 덤프에서도 같은 오류가 났습니다.

### 문서
- README, 한국어 README, Nexus 설명의 수동 스냅샷 단축키 옆에, Crash Logger 1.25도 같은 키로 스레드 덤프를 만든다는 안내를 추가했습니다.

### 주의사항
- 플러그인(`SkyrimDiag.dll`), Helper, 분석기 CLI가 바뀌었습니다. zip 전체를 교체해 주세요.
- Crash Logger가 SkyrimDiag가 기록하거나 보관하지 않는 드문 예외(예: 처리되지 않은 `0xC0000008`)를 보고하는 경우에는, 이전처럼 Crash Logger의 조사 예외가 대신 기록되지 않아 SkyrimDiag 덤프가 남지 않습니다. 그 덤프는 원인 위치가 Crash Logger로 잘못 찍힌 것이었고, Crash Logger 로그는 그대로 남습니다.
- `ERROR_PARTIAL_COPY`의 원인은 아직 찾지 못했고, 로컬에서는 재현하지 못했습니다(메모리·스레드 변동, 스택 안의 접근 불가·가드 페이지 모두 덤프 성공). 재시도는 이 플래그의 문서화된 동작에 기댑니다. 재시도로 남은 덤프는 읽지 못한 메모리 영역이 빠져 있을 수 있습니다.

### 테스트
- Windows 전체 테스트: `74/74` 통과.
- 실제 플러그인 예외 처리기와 테스트용 `CrashLogger.dll`로, 보관한 예외 없이 Crash Logger 조사 예외만 날 때 기록되지 않고 캡처가 준비 상태로 남는지, 그 뒤 실제 assert 크래시는 정상 기록되는지 확인하는 런타임 테스트를 추가했습니다.
- Windows 언어 ID별 리포트 언어 판정, 두 WER 힌트 문구의 차이, 부분 복사 재시도 판정에 대한 테스트를 추가했습니다.
- 실사고 hang 덤프(SmoothCam 오류 창)를 `--lang` 없이 헤드리스로 재분석해 한국어 리포트가 나오는 것을 확인했습니다.

## v0.2.59-rc4 (2026-10-05)

### 한눈에 보기
- rc3 실게임 테스트의 CTD 리포트에서 드러난 문제를 고쳤습니다. rc3에서 크래시 후 플러그인 스캔이 살아나자, MO2 + Anniversary Edition 환경에서 Creation Club 파일이 모두 "누락 마스터"로 잘못 판정되어 리포트 맨 위 `[필수]` 안내와 NextAction을 차지했습니다.
- PDB가 없는 모드 DLL의 콜스택이 `SmoothCam.dll!SKSEPlugin_Load+0x7bd57`처럼 엉뚱한 함수 이름으로 표시되던 문제를 고쳤습니다.

### 수정
- **게임이 스스로 읽는 플러그인을 스캔에 포함** — 게임은 기본 마스터(Skyrim.esm, Update.esm, DLC 3개)와 `Skyrim.ccc`에 적힌 Creation Club 파일을 `plugins.txt` 항목 없이 읽는데, 모드 관리자는 보통 이들을 `plugins.txt`에 넣지 않습니다. Helper의 플러그인 스캔이 `Data`에 실제로 있는 이 파일들을 활성 플러그인으로 포함하고, 그렇게 했다는 표시(`implicit_plugins_included`)를 남깁니다.
- **이전 스캔 결과의 CC 마스터 오판 방지** — 이 표시가 없는 이전 버전의 스캔 결과는 어떤 CC 파일이 실제로 로드됐는지 알 수 없으므로, `plugins.txt`에 없는 CC 마스터를 누락으로 판정하지 않습니다. 이전 덤프를 다시 분석해도 오판이 나지 않습니다.
- **신뢰도 낮은 플러그인 규칙은 체크리스트 뒤로** — "ESL 슬롯 한계 근접" 같은 신뢰도 낮은 규칙의 안내가 크래시 안내보다 앞서 NextAction이 되지 않도록, 체크리스트 끝으로 보냅니다. 높은·중간 신뢰도 규칙의 위치는 그대로입니다.
- **PDB 없는 DLL의 함수 이름** — PDB가 없으면 DbgHelp는 주소를 그 아래 가장 가까운 공개(export) 함수 이름으로 부릅니다. 그 거리가 `0x1000`을 넘으면 함수 이름 대신 `모듈+오프셋`으로 표시합니다.

### 테스트
- Windows 전체 테스트: `74/74` 통과.
- 임시 MO2 구성 폴더로 플러그인 스캔을 실행해 기본 마스터·`Skyrim.ccc` 파일 포함, 설치되지 않은 파일 제외, 중복 제거, 표시 기록을 확인하는 테스트를 추가했습니다.
- 이전 형식과 새 형식 스캔 결과의 누락 마스터 판정, 신뢰도 낮은 규칙의 순서에 대한 테스트를 추가했습니다.
- rc3 실사고 덤프(SmoothCam CTD) 재분석: 누락 마스터 오판이 사라지고, NextAction이 접근 위반 안내로 돌아오며, 콜스택이 `SmoothCam.dll+0x...`로 표시됩니다. 실제 MO2 구성의 모드 폴더 기준으로 계산하면 이 사용자의 누락 마스터는 136개에서 0개가 됩니다.

### 주의사항
- 이번 변경은 Helper와 분석기입니다. 플러그인(`SkyrimDiag.dll`)은 rc3과 같지만 zip 전체를 교체해 주세요.
- 플러그인 스캔은 MO2 가상 파일 시스템 안에서 실행되는 Helper가 `Data`를 보는 방식에 기대며, 이 사용자의 환경에서 Helper가 모드 폴더의 플러그인 헤더를 읽는 것을 확인했습니다.

## v0.2.59-rc3 (2026-10-05)

### 한눈에 보기
- rc2 실게임 테스트에서 나온 CTD를 바로잡았습니다. 모드의 assert가 일으킨 크래시를 CrashLogger가 기록하는 동안 CrashLogger 내부에서 난 2차 예외가 크래시로 기록되어, 덤프와 리포트가 원인 모드 대신 `CrashLogger.dll`을 가리키던 문제입니다.
- 콜스택에 코드가 아닌 주소(`.rdata`의 vtable 등)가 프레임으로 섞이던 문제와, 크래시 후 플러그인 스캔이 항상 건너뛰어지던 문제를 고쳤습니다.

### 수정
- **CrashLogger가 보고 중인 원래 예외를 기록** — 치명 예외만 기록하는 기본 모드(`CrashHookMode=1`)는 assert/abort의 브레이크포인트(`0x80000003`)와 C++ 예외(`0xE06D7363`)를 보통 처리되는 예외로 보고 기록하지 않습니다. 이런 예외가 처리되지 않으면 CrashLogger가 보고하는데, 그 과정에서 스스로 잡는 접근 위반을 내고, 기존에는 이것이 크래시로 기록됐습니다. 이제 플러그인이 기록하지 않은 마지막 브레이크포인트·C++ 예외를 스레드와 함께 보관해 두었다가, 아무것도 기록되지 않은 상태에서 같은 스레드의 CrashLogger 내부 예외가 오면 보관한 예외를 크래시로 기록합니다. CrashLogger가 직접 던진 C++ 예외는 보관하지 않고, 이후 CrashLogger 내부 예외는 기존처럼 무시합니다.
- **CrashLogger 내부 예외의 해석 안내** — 예외 위치가 `CrashLogger.dll`/`CrashLoggerSSE.dll`이면 일반 훅 프레임워크 안내 대신, 다른 크래시를 기록하다 난 2차 예외일 가능성이 크니 짝지어진 CrashLogger 로그의 원래 예외와 콜스택을 보라고 안내합니다. 이전 버전으로 캡처한 덤프에도 적용됩니다.
- **브레이크포인트 예외 안내** — `0x80000003`이면 모드가 내부 검사(assert/abort)에 실패해 스스로 멈춘 경우가 많다는 설명과 함께, 콜스택에서 시스템 DLL이 아닌 첫 모듈을 먼저 확인하라고 안내합니다.
- **콜스택에 코드가 아닌 주소를 넣지 않음** — 0번 프레임이 아닌 호출자 주소가 로컬 이미지에서 실행 가능한 섹션 밖(예: `.rdata`)이면 그 프레임을 넣지 않고 멈춥니다. 덤프를 쓰는 동안 스레드가 계속 실행되어 스택이 덮인 경우에 생기던 쓰레기 프레임입니다.
- **크래시 후 플러그인 스캔** — 크래시 확정 과정은 게임이 종료될 때까지 기다리는데, 플러그인 스캔은 그 뒤에 실행 파일 경로와 모듈 목록을 읽으려다 실패해 `PluginScanner skipped: failed to resolve game exe directory.`만 남겼습니다. 이제 덤프를 쓴 직후 게임이 살아 있을 때 이 정보를 읽어 둡니다.

### 주의사항
- 플러그인(`SkyrimDiag.dll`)이 바뀌었습니다. 공유 메모리 프로토콜(SharedLayout v4)은 그대로지만 zip 전체를 교체해 주세요.
- 원래 예외를 되살리는 것은 CrashLogger가 설치되어 있고 같은 스레드에서 보고할 때만 동작합니다. CrashLogger 없이 처리되지 않은 브레이크포인트로 종료되는 경우는 이번에 바꾸지 않았습니다.
- 수정 근거는 실사고 덤프 1건(rc2 실게임 테스트)과 그 CrashLogger 로그입니다. 이 덤프는 이미 CrashLogger 내부 예외로 기록되어 있어, 원래 예외 기록은 같은 순서를 재현하는 런타임 테스트로만 확인했습니다.

### 테스트
- Windows 전체 테스트: `73/73` 통과.
- 실제 플러그인 예외 처리기와 `CrashLogger.dll`이라는 이름의 테스트용 DLL로, assert 브레이크포인트 → CrashLogger 내부 접근 위반 순서를 재현하는 런타임 테스트를 추가했습니다. 수정을 끄면 실패하고 켜면 통과합니다.
- 로컬 이미지의 코드 섹션 판정, 예외 코드별 권장 조치, 플러그인 스캔 입력 수집 시점에 대한 테스트를 추가했습니다.
- rc2 실사고 덤프 재분석: 콜스택의 `SkyrimSE.exe+0x1aef620`(`.rdata`) 프레임이 사라지고, CrashLogger 2차 예외 안내가 표시됩니다.

## v0.2.59-rc2 (2026-10-02)

### 한눈에 보기
- 플러그인이 띄운 오류 대화상자 때문에 게임이 멈춘 경우를 **`modal_dialog_wait`** 상태로 분류하고, 스택에 우연히 남은 무관한 플러그인을 원인으로 지목하지 않습니다(#3).
- 정식 stackwalk가 처음부터 **항상 1프레임에서 멈추던 버그**를 고쳐, CTD와 프리징 리포트에 실제 호출 체인이 나타납니다.
- Linux(Wine/Proton)에서는 동작하지 않는 WinUI 뷰어를 기다리지 않고, 모든 캡처에 텍스트 리포트를 남깁니다(#5).
- MO2에서 사고 후 결과가 생기지 않을 때의 확인 방법과 실제 폴더 배치 우회법을 문서에 추가했습니다(#4).

### 수정
- **modal 대화상자 프리징 분류** — Helper가 hang 캡처 시 대상 프로세스의 보이는 대화상자(`#32770`: MessageBox, DialogBox, TaskDialog)의 소유 스레드, 제목, 본문을 WCT JSON의 `modal_dialogs`에 기록합니다. 대상에 메시지를 보내지 않으므로 멈춘 게임이 Helper를 막지 못합니다. 분석기는 메인 스레드가 그 대화상자를 소유하는지(창 근거)와, 메인 스레드 스택이 `user32` MessageBox/DialogBox 또는 `comctl32` TaskDialog 진입점에서 대기 중이고 그 위에 플러그인 코드가 없는지(스택 근거)를 각각 판정합니다. 둘 다 성립하면 `High`, 하나면 `Medium`이며, 다른 프리징 상태보다 먼저 판정합니다.
- **modal 대기의 후보 귀속** — 대화상자 API를 직접 호출한 일반 플러그인만 `modal_dialog_owner` 후보(`related / Medium`)가 됩니다. SKSE 런타임, 훅 프레임워크, 게임 EXE가 띄운 대화상자는 그렇게 설명만 하고 스택 아래쪽 모듈로 넘기지 않습니다. Crash Logger frame, 스택 밀도, 스레드 그룹, 인접 리소스 후보는 만들지 않고, 스택 suspect는 `Low`로 낮춰 원인이 아님을 표시합니다. 요약 문장, 근거 항목, 텍스트 리포트, `freeze_analysis.modal_dialog_wait` JSON, 권장 조치("게임 창 뒤 오류 창 확인"과 캡처된 대화상자 본문), WinUI 다음 행동 안내에 반영했습니다. 본문의 사용자 프로필 경로 이름은 `<user>`로 가립니다.
- **정식 stackwalk가 실제 호출자를 복원** — 기존 루프는 `StackWalk64`의 첫 호출(컨텍스트 자신의 프레임)을 진전 없음으로 보고 멈춰, CTD는 fault 프레임 하나만 점수에 쓰였고 프리징은 사실상 항상 메인 스레드 포인터 스캔으로 넘어갔습니다. 이제 모든 프레임을 unwind하며, 덤프에 없는 모듈 메모리와 `.pdata`는 로컬 이미지 파일의 `TimeDateStamp`와 `SizeOfImage`가 덤프 모듈과 같을 때만 읽습니다. 0번 프레임 외에는 unwind 항목이 없으면 멈추고 모듈 밖 주소를 기록하지 않아, 스택 슬롯 추측으로 힙·전역·vtable 주소가 프레임에 섞이지 않습니다.
- **프리징 stackwalk 대상 제한** — 메인 스레드를 아는 프리징은 메인 스레드와 WCT cycle 스레드만 stackwalk합니다(ADR-0005). 자기 루프에서 쉬고 있는 워커 스레드는 정지의 근거로 쓰지 않습니다.
- **stackwalk 진단 구분** — Helper의 재수집 상향 판단에 쓰이는 `[Stackwalk] DbgHelp stackwalk failed`는 실제로 호출자 프레임을 얻지 못한 경우에만 남깁니다. 프레임은 얻었지만 실행 가능한 모듈이 없으면 `formal stackwalk found no actionable module`로 구분하고, 프리징 요약도 "실패" 대신 "실행 가능한 모듈을 특정하지 못함"으로 표시합니다.
- **Wine/Proton에서 헤드리스 리포트 보장** — WinUI 뷰어는 Wine이 제공하지 않는 Windows App Runtime 구성 요소가 필요해 시작 단계에서 실패하지만, 그 프로세스가 Helper의 실행 확인 시간보다 오래 살아 있어 헤드리스 분석이 건너뛰어졌습니다. Helper가 Wine을 감지하면 뷰어 자동 열기를 끄고 모든 캡처에 `*_SkyrimDiagReport.txt`를 생성하며, `SkyrimDiagHelper.ini`의 `AutoOpenViewerUnderWine=1`로 이 동작을 끌 수 있습니다. 뷰어 런처는 Wine에서 WinUI 앱을 띄우지 않고 이유를 안내하며, 덤프가 주어지면 함께 배포된 CLI로 리포트를 만든 뒤 열지 묻습니다.

### 빌드·검증
- **crash bucket v3(`CTD3-`)** — 버킷 키를 이루는 stackwalk 프레임이 달라지므로 버킷 버전을 올렸습니다.
- **분석기 데이터 복사 경쟁 제거** — `SkyrimDiagDumpToolNative.dll`과 `SkyrimDiagDumpToolCli.exe`가 각각 POST_BUILD 단계에서 같은 `data` 폴더로 복사하다 동시에 끝나면 빌드가 실패하던 문제를, 단일 `SkyrimDiagDumpToolData` 타깃으로 고쳤습니다.
- **CI 중복 실행 제거** — PR 브랜치는 `pull_request`로 한 번만 검증하고, 같은 PR에 새로 푸시하면 진행 중인 이전 실행을 취소합니다. `main` 푸시는 취소하지 않습니다.
- **테스트 안정화** — 버튼 하나짜리 MessageBox의 OK 버튼 ID가 `IDCANCEL`이라 `IDOK`로 닫히지 않던 테스트를 `WM_CLOSE`와 제한 시간으로 고쳤고, 덤프 대상 자식 프로세스가 초기화를 마칠 때까지 기다려 `ERROR_PARTIAL_COPY` 간헐 실패를 막았습니다.
- 설계 기록으로 ADR-0006(modal 대화상자 분류)과 ADR-0007(정식 stackwalk unwind)을 추가했습니다.

### 문서
- **MO2 실제 폴더 배치 안내** — 모드로만 설치했을 때 사고 후 덤프·리포트가 생기지 않거나 뷰어가 열리지 않으면 `SkyrimDiagHelper.log`를 먼저 확인하고, `SKSE\Plugins\` 전체를 실제 `Data` 폴더에 복사하는 사용자 제보 우회법을 README·한국어 README·Nexus 설명에 추가했습니다. 한국어 README의 Helper 로그 경로도 실제 위치(`Tullius Ctd Logs\SkyrimDiagHelper.log`)로 바로잡았습니다.
- **Linux(Wine/Proton) 지원 범위** — 캡처와 텍스트 리포트는 동작하고 WinUI 뷰어는 동작하지 않는다는 점, 이전 버전용 설정(`AutoOpenViewerOnCrash=0`, `AutoOpenViewerOnHang=0`)을 문서에 추가했습니다.

### 주의사항
- 새 `CTD3-` 버킷은 기존 `CTD2-` history 그룹과 자동으로 합쳐지지 않습니다.
- stackwalk는 덤프의 모듈과 같은 버전의 파일이 분석하는 PC에 있어야 그 모듈을 지나 내려갈 수 있습니다. 삭제·업데이트된 모드나 다른 PC에서 분석한 덤프는 해당 모듈에서 멈추고, modal 판정도 창 근거만 남을 수 있습니다.
- 실제 호출 체인이 나타나면서 후킹 체인(D3D 초기화 훅 등)에 놓인 다른 DLL도 호출자로 보일 수 있습니다. 점수 가중치와 후보 정책은 바꾸지 않았으므로 이런 DLL은 `Low` 보조 후보로만 남습니다.
- 이번 동작 변경은 한 사용자 PC의 실사고 덤프 7개(crash 2, hang 5)로 회귀만 확인했으며, 정확도 측정이 아닙니다. 검수된 실사고 코퍼스가 없으므로 다른 로거 대비 적중률을 수치로 주장하지 않습니다.
- Wine에서는 WCT 대기 체인과 modal 대화상자 스택 근거를 쓸 수 없고 심볼·소스 줄 정보가 줄어듭니다. Helper의 Wine 감지는 실제 Proton 게임 세션에서 확인하지 못했습니다.
- 플러그인과 공유 메모리 프로토콜(SharedLayout v4)은 바뀌지 않았지만, Helper·분석기·WinUI 런처가 함께 바뀌므로 zip 전체를 교체해 주세요.

### 테스트
- Windows 전체 테스트: `70/70` 통과. modal 대화상자 판정·WCT 파싱 단위 테스트, 실제 MessageBox를 Helper로 캡처하는 테스트, Helper hang 캡처부터 분석기까지 이어지는 modal E2E 테스트, 세 단계 호출 체인을 복원하는 stackwalk E2E 테스트를 추가했습니다.
- Linux 전체 테스트: `62/62` 통과.
- Windows production clang-tidy 전체: clean(각 PR 기준).
- Wine 9.0 실측: CLI 분석, Helper CTD·hang 캡처, 런처의 Wine 안내와 리포트 생성을 확인했습니다.
- 실사고 덤프 7개 전후 비교: crash 2건은 최상위 후보 유지, hang 4건은 "stackwalk 실패"에서 실제 메인 스레드 호출 체인 복원, 나머지 1건은 GPU 드라이버 안 대기까지 복원했습니다.

## v0.2.59-rc1 (2026-08-08)

### 수정
- **CTD 후보 신뢰도 보수화** — Crash Logger frame과 Tullius stack이 같은 사고의 실행 위치를 함께 가리키더라도 이를 독립 교차검증으로 계산하지 않고, `related / Medium`의 fault-location 보강으로 해석합니다. `High`는 boost-only인 history/resource가 아니라 strong object ref 같은 비스택 의미 근거와 정식 actionable stack의 두 weight 합이 10 이상일 때만 허용합니다.
- **보강 신호의 승격 차단** — 반복 이력, 인접 리소스 제공자, first-chance 문맥은 후보 설명과 순위 보강에는 남기되 `High` 임계값의 부족분을 채우지 못하게 했습니다. hang thread-group 합의는 ADR-0005의 `synchronization_stall_likely / Medium` 계약을 유지합니다.
- **사용자 문구 정렬** — 동일 사고의 frame+stack 조합을 `교차검증된 원인`으로 부르던 품질 코퍼스와 공유 텍스트를 `현재 fault location을 상호 확인한 단서`로 낮춰, 원인 확정과 실행 위치 보강을 구분합니다.
- **프리징 원인 귀속의 기준을 메인 스레드로** — 프리징 분석의 기준 스레드를 blackbox의 최신 `Heartbeat` 이벤트 스레드(없으면 `SessionStart`)로 정하고, Helper의 hang 덤프와 분석기 모두 이 메인 스레드를 우선합니다. 스레드를 많이 만든 모드나 직전에 로드된 리소스 제공자가 WCT 후보 스레드 여러 개의 포인터 스캔으로 원인처럼 과대평가되던 문제를 줄입니다. 정식 stackwalk가 실패하면 포인터 스캔은 메인 스레드만 대상으로 하며, 결과는 항상 `Low`의 약한 단서로 표시합니다(ADR-0005).
- **스레드 그룹 합의(`synchronization_stall_likely`)** — 메인 스레드를 포함한 4개 이상 스레드의 현재 스택 상단 32슬롯에 같은 모듈이 있고, 두 WCT 캡처 사이 그 스레드들의 context-switch 수가 모두 변하지 않으면 모듈 수준 동기화 정지로 보고 `Medium`을 부여합니다. WCT가 실제 순환 대기를 보고하지 않았다면 OS 잠금 사이클이 입증됐다고 표현하지 않습니다. 요약 JSON의 `freeze_analysis.thread_module_consensus`, 텍스트 리포트, WinUI("메인 스레드 + 정지 워커 그룹")에 반영했습니다.
- **리소스 제공자 단독 후보 금지** — 인접 리소스 제공자 신호는 다른 실행 근거를 보강할 수만 있고, 단독으로 실행 우선 후보나 프리징의 다음 행동이 되지 않습니다.

### 테스트
- 합성 품질 코퍼스의 direct DLL/system victim/hook-framework victim 사례를 `related / Medium`으로 고정하고, strong CrashLogger object ref와 정식 actionable stack의 두 weight 합이 10 이상인 별도 High 양성 대조군을 추가했습니다.
- 메인 스레드 우선 선택, 메인 스레드 한정 포인터 스캔, 리소스 단독 후보 억제, WCT 두 캡처 간 context-switch 안정성 판정, 스레드 그룹 합의 상태를 단위·회귀 테스트로 고정했습니다.

## v0.2.58 (2026-07-28)

### 한눈에 보기
- 이번 릴리즈는 **CTD 증거가 조용히 사라지는 경로를 막고, 릴리즈·CI 검증 체계를 실제로 동작하게 연결**합니다.
- 덤프 쓰기가 일시적으로 실패해도 사고를 잃지 않으며, 정상 종료(exit 0)로 삭제되는 실제 결함은 메타데이터만이라도 남깁니다.
- 프로세스 종료 직전 경합, 산출물 삭제 결과, 손상된 상태 JSON, ETW 종료 실패까지 사고 종료 경로의 최종 상태를 사실대로 남깁니다.
- CrashLogger가 우리보다 늦게 로드되는 로드오더에서도 중첩 예외 억제가 동작합니다.
- 새 기능보다 **기존 기능이 실제로 검증되는지**에 무게를 둔 릴리즈입니다.

### 수정
- **덤프 쓰기 재시도** — 크래시 이벤트는 덤프 기록 전에 소비되고 같은 결함으로 다시 신호되지 않으므로, 일시적 쓰기 실패는 곧 사고 유실이었습니다. 대상 프로세스가 살아 있는 동안 제한된 횟수만큼 그 자리에서 재시도합니다.
- **정상 종료 증거 격리(신규, 기본 켜짐)** — exit 0은 예외가 처리되었다는 신호로 보고 덤프를 삭제하지만, 외부 크래시 핸들러의 `ExitProcess(0)`이나 종료 코드를 정규화하는 런처 때문에 실제 CTD가 0으로 끝나는 경우가 있습니다. 강한 결함이 이미 게시되었고 하트비트 복구가 관측되지 않았다면 초기 필터와 지연된 프로세스 종료 경로 모두에서 `SkyrimDiag_CleanExitEvidence_*.json`을 남깁니다. 2단계 disposition으로 먼저 덤프 identity와 보존 정책을 고정하고, 삭제를 시도한 뒤 `dump_preserved`, `delete_failed`, 실제 파일 상태를 원자적으로 확정해 기록합니다. 기본 설정에서는 덤프와 파생 리포트를 삭제하고, `PreserveFilteredCrashDumps=1`이면 덤프는 남기되 파생 리포트와 자동 동작만 억제합니다. JSON 쓰기가 실패하면 증거를 모두 잃지 않도록 덤프를 자동 보존합니다. `SkyrimDiagHelper.ini`의 `EnableCleanExitEvidenceQuarantine`으로 JSON 기록을 끌 수 있습니다.
- **exit-0 최종 폴 경합 제거** — 프로세스 종료를 관측한 직후 마지막 crash-event poll에서 새 결함 메타데이터가 발견돼도 이미 죽은 프로세스에 `MiniDumpWriteDump`를 시도하지 않습니다. 종료 후 drain은 metadata-only로 수행하고 동일 사고의 증거 판정에만 합칩니다.
- **헬퍼 JSON 원자 기록과 손상 통계 격리** — incident/clean-exit/adaptive-load/crash-bucket JSON은 임시 파일을 완성한 뒤 교체해 중간 내용이 정상 상태처럼 노출되지 않게 했습니다. crash-bucket stats가 손상됐으면 기본값으로 조용히 덮지 않고 별도 corrupt 파일로 격리한 뒤 새 상태를 시작합니다.
- **링 엔트리별 seqlock 스냅샷** — 전체 crash record가 안정적이어도 동시에 쓰는 blackbox/resource 엔트리가 찢어질 수 있었습니다. Helper가 각 엔트리의 전후 sequence를 검증해, 겹친 엔트리만 명시적으로 무효화하고 나머지 사고 스냅샷은 보존합니다.
- **ETW 종료의 제한 재시도와 정리 상태** — `wpr -stop` 실패를 한 번의 성공/실패로 축약하지 않고 제한된 횟수로 재시도합니다. 끝내 확인되지 않으면 `wpr -cancel` 결과까지 확인하고, 정리가 확정되지 않은 경우 manifest에 `cleanup_unconfirmed`를 남겨 완료된 ETL처럼 취급하지 않습니다.
- **덤프 identity 기반 산출물 충돌 방지** — 같은 stem을 가진 다른 덤프의 authoritative 산출물을 덤프 identity가 붙은 report/summary 쌍으로 분리하며, WinUI triage 저장도 동일 identity 계약을 사용합니다. 기존 이름의 파일은 호환용 최신 별칭일 뿐 사건 identity의 저장소로 사용하지 않습니다.
- **동시 분석 출력 직렬화** — CLI와 WinUI가 같은 stem을 동시에 분석해도 per-stem 파일 잠금 안에서 report/blackbox/WCT/Summary 세대를 게시합니다. WinUI triage 저장도 같은 잠금을 사용하며 summary mirror 실패 시 authoritative triage state를 보상 복구합니다.
- **열린 덤프 handle에 identity 결합** — 해시는 mmap한 파일인데 크기·FILETIME은 교체된 경로에서 읽는 경합을 없앴습니다. SHA-256, 크기, FILETIME, volume/file ID를 동일한 열린 handle에서 해시 전후 확인합니다.
- **손상된 정상 종료 sidecar 격리** — 문법은 맞지만 필드 타입이 잘못된 clean-exit JSON도 optional evidence로 안전하게 거부하며 CLI 분석 밖으로 예외를 전파하지 않습니다.
- **플러그인 worker 종료 안전성** — DLL detach의 loader lock 안에서 `jthread` 소멸자가 join하지 않도록 worker 시작 전에 모듈을 pin하고, 일반 스레드에서 호출할 명시적 shutdown 경계를 제공합니다.
- **늦게 로드되는 CrashLogger 대응** — CrashLogger도 SKSE 플러그인이라 우리 뒤에 로드될 수 있고, 그러면 설치 시점 조회 결과가 비어 중첩 결함 억제가 영구히 꺼졌습니다. SKSE `kPostLoad` 시점부터 모듈 범위를 다시 조회하고, 크래시 핸들러가 찢어진 범위를 절대 관측하지 않도록 write-once 게시 규약으로 공개합니다.
- **양성 예외 코드 분류 보정** — 싱글 스텝, 스레드 이름 설정, `OutputDebugString` 예외를 강한 결함 분류에서 제외해 unsafe `CrashHookMode=2`에서 이런 정상 알림이 clean-exit 증거 JSON의 대상이 되지 않게 했습니다. 기본 `CrashHookMode=1`의 치명적 예외 선택은 기존과 같습니다.
- **분석기 이식성/불필요 복사 수정** — `Mo2Index`의 경로 사본 2곳을 참조로 바꾸고, 상위 코드 유닛이 부호 확장되던 `wchar_t` 폭 확장 경로를 부호 없는 등가 타입 경유로 고쳤습니다.

### 빌드·검증
- **하나의 버전 원천과 실제 바이너리 검증** — `CMakeLists.txt`의 `0.2.58`에서 SKSE `PluginDeclaration`, plugin/helper/CLI/native/launcher의 `VERSIONINFO`, WinUI assembly/file/product version과 application manifest를 생성합니다. `vcpkg.json`에 별도 버전이 있으면 반드시 일치해야 하며, 릴리즈 게이트는 ZIP 안 실제 PE fixed metadata와 SKSE export를 읽어 확인합니다.
- **commit-bound 빌드·패키지 provenance** — native/WinUI 빌드는 HEAD, dirty 여부, 소스 트리 fingerprint와 정확한 산출물 해시를 manifest에 기록합니다. 패키지는 두 build manifest와 ZIP의 모든 파일 해시를 결합하며, recursive/mtime 후보 검색 없이 지정 configuration의 단일 경로만 허용합니다. 로컬 dirty 패키지는 진단용으로 허용하지만 CI와 공개 릴리즈는 `git_dirty=false`를 강제합니다.
- **WinUI 출력 경로 환경 독립화** — GitHub MSVC 환경이 `Platform=x64`를 주입해도 로컬과 같은 산출물 계약을 사용하도록 `dotnet publish`에 x64 platform을 명시합니다. 해당 단일 `bin\x64\Release`·`obj\x64\Release` 트리를 publish 전에 지우고 그 실행에서 생성된 XAML만 staging에 합칩니다.
- **실제 ZIP 진입점 스모크** — 릴리즈 ZIP을 새 임시 폴더에 풀고 packaged top-level WinUI launcher에 Windows가 생성한 유효 minidump를 전달합니다. exit 0과 identity-aware report/summary JSON 쌍 생성을 모두 확인하며, DLL 직접 실행이나 missing-dump 오류 경로를 성공으로 대신하지 않습니다.
- **전체 PE 아키텍처와 prerelease 상태 검증** — 고정된 핵심 파일뿐 아니라 ZIP의 모든 EXE/DLL을 검사해 native x64, x64 managed, AnyCPU IL-only, 실제 hybrid metadata가 있는 ARM64X만 허용합니다. suffix가 붙은 허용 태그는 모두 GitHub prerelease로 만들고 공개 readback 상태도 양방향 비교합니다.
- **clang-tidy 전체 production 커버리지** — Linux의 빠른 부분 집합에 더해 Windows Ninja compile database가 `dump_tool/src`, `helper/src`, `plugin/src`의 실제 production `.cpp`와 생성된 plugin metadata 소스를 모두 포함하는지 검증하고, 전체 집합을 `WarningsAsErrors`로 실행합니다. 커버 파일 목록을 CI 로그에 출력합니다.
- **퍼저를 CI에서 실제 실행** — 크래시 로그 파서는 다른 모드가 쓴 파일을 읽는, 이 프로젝트에서 가장 신뢰할 수 없는 입력을 다룹니다. `crashlogger`/`wct` 파서 퍼저를 CI에서 실행하고, libFuzzer가 새 입력을 첫 번째 코퍼스 인자에 쓰므로 스크래치 디렉터리를 앞에 두어 검수된 시드 코퍼스가 오염되지 않게 했습니다.
- **Windows 테스트를 CI에서 실행** — 헬퍼·플러그인 런타임 테스트는 Windows에서만 빌드되므로, 그동안 로컬 실행에만 의존하고 있었습니다.
- **분석기 동작 회귀 게이트 신설** — `skydiag_quality_corpus_runner`가 raw `CandidateSignal` 픽스처를 production `BuildCandidateConsensus()`에 통과시켜 후보 순위·상태·신뢰도·점수·기권을 생성하고, `skydiag_quality_corpus_gate_tests`가 그 임시 Summary만 품질 채점기에 전달합니다. 미리 계산된 Summary는 소스 코퍼스에 둘 수 없습니다. 이는 **실사고 정확도 측정이 아니라 candidate-consensus 동작 회귀 감지**이며, 정확도 주장은 릴리즈 게이트의 별도 reviewed-corpus 단계가 계속 담당합니다.
- **미측정 상태를 명확히 보고** — 검수된 실사고 코퍼스가 없으면 릴리즈 게이트가 "이번 릴리즈의 실사고 귀속 정확도는 미검증"이라고 명시적으로 출력합니다. 통과로 위장하지 않습니다.
- **CI 배선 자체를 지키는 테스트** — `skydiag_ci_wiring_tests`가 clang-tidy·퍼저·Windows ctest 호출이 워크플로에서 사라지면 실패합니다.
- **태그 릴리즈도 전체 Linux 게이트 실행** — 일반 CI가 버전 태그를 제외하므로, 릴리즈 워크플로가 unit·ASan+UBSan·clang-tidy·parser fuzz를 직접 다시 실행한 뒤 Windows 빌드/패키징으로 넘어갑니다.
- **Windows 테스트 이식성 보정** — source/XAML guard는 CRLF를 LF로 정규화하고, .NET share-text fixture의 stdout은 UTF-8로 명시합니다. clang-tidy compile-database 검사는 8.3 short path나 `alias\..`가 같은 저장소를 가리킬 때 먼저 canonicalize해 runner 경로 표기 차이를 소스 누락으로 오인하지 않습니다.

### 주의사항
- **SharedLayout protocol v4는 Plugin과 Helper를 함께 교체해야 합니다.** Plugin은 `kState_Frozen`의 clear→set CAS를 단일 사고 ownership 지점으로 사용해 최초 강한 결함을 고정하고, Helper만 명시적으로 reject/abandon한 세대를 ACK/reset해 다음 사고가 슬롯을 claim하도록 합니다. 동시성·호환성 회귀는 자동 테스트로 검증했고, 사용자의 RC3 기본 실사용 스모크에서는 이상이 보고되지 않았습니다. 다만 실제 Skyrim 런타임의 연속 first-chance/CTD와 구버전 save·모드 조합은 체계적으로 확인하지 않았습니다.
- 정상 종료 증거 격리는 기본적으로 JSON 메타데이터만 남깁니다. 덤프가 필요하면 `PreserveFilteredCrashDumps=1`을 켜야 하며, 이 경우 JSON도 덤프가 보존됐다고 명시합니다.
- 검수된 실사고 코퍼스가 여전히 없으므로, 다른 크래시 로거 대비 적중률을 수치로 주장하지 않습니다.
- 플러그인, Helper, 분석기와 WinUI가 함께 바뀌므로 이전 릴리즈 파일과 섞지 말고 zip 전체를 업데이트해 주세요.

### 테스트
- 릴리즈 ZIP 검증기·패키징 계약·CI 배선 회귀 테스트: 통과.
- Windows native build와 이전 산출물을 재사용하지 않는 WinUI fresh staging publish: 성공.
- Windows 전체 테스트: `67/67` 통과.
- Linux 새 구성·빌드와 전체 테스트: `61/61` 통과.
- Windows production compile database 전체 `86`개 번역 단위 clang-tidy(`WarningsAsErrors`): clean.
- 현재 통합 소스의 로컬 diagnostic ZIP·release gate·packaged-launcher smoke는 clean provenance를 강제해 최종 재실행하며, 태그 워크플로는 공개 asset 재다운로드와 SHA-256 일치까지 확인해야 완료됩니다.
- 사용자 RC3 기본 실사용 스모크에서는 이상이 보고되지 않았습니다.
- 실사고 품질 코퍼스, 연속 first-chance/CTD, 구버전 save·모드 조합은 미검증입니다.

## v0.2.57 (2026-07-23)

### 한눈에 보기
- 이번 릴리즈는 **CrashLogger SSE v1.24 실사고 호환성과 CTD 로그 화면의 읽기 흐름을 함께 개선**합니다.
- 일반 런타임 `CrashLogger.log`를 실제 크래시 로그로 잘못 페어링하지 않으며, 원래 예외가 기록된 뒤 CrashLogger 내부에서 발생한 후속 예외가 크래시 컨텍스트를 덮지 않도록 했습니다.
- 덤프의 표면상 크래시 위치가 CrashLogger이더라도 페어링된 로그에 행동 가능한 비훅 DLL 프레임이 있으면, 해당 후보를 원인 확정이 아닌 우선 점검 대상으로 안내합니다.

### 수정
- **CrashLogger v1.24 로그 판별 강화** — `Thread dump` 문구만 있는 런타임 로그는 제외하고 실제 `Callstack:` 섹션까지 있는 크래시 아티팩트만 페어링 후보로 사용합니다.
- **원래 예외 컨텍스트 보존** — 핸들러 설치 시 CrashLogger 모듈 범위를 캐시하고, 이미 크래시가 고정된 뒤 해당 범위에서 발생한 후속 예외는 최초 컨텍스트를 교체하지 못하게 했습니다.
- **훅 프레임워크 요약 보정** — raw dump가 CrashLogger 같은 훅 프레임워크를 가리킬 때 페어링된 Crash Logger 프레임 기반 비훅 DLL 후보를 우선 점검 대상으로 표시하되, 피해 위치일 가능성과 중간 이하의 신뢰도 표현을 유지합니다.
- **실제 형식 회귀 테스트** — 개인정보를 제거한 CrashLogger v1.24 실제 형식 fixture를 추가하고 런타임 로그 오선택, 직접 오류 프레임, 검색 통합 경로를 검증합니다.
- **WinUI 반응형 배치** — 창의 실제 viewport 너비를 사용하고 좁은 창에서는 CrashLogger 기준, 근거 합의, 다음 조치 카드를 세로로 배치합니다.
- **WinUI 빈 상태와 읽기 순서** — 분석 전과 원시 데이터 없음 상태를 별도 안내하며, 크래시 맥락과 권장 조치 뒤에 검토 피드백이 오도록 화면 순서를 정리했습니다.
- **한국어 문구 정리** — Tullius 콜스택, DLL 점검 안내, 선행 예외, 리포트/원시 데이터와 시작 오류 문구를 자연스러운 한국어로 통일했습니다.

### 주의사항
- Crash Logger 프레임과 Tullius 후보는 원인 확정이 아니라 우선 점검 근거입니다. 검토 완료 실사고 코퍼스가 없으므로 다른 크래시 로거보다 높은 적중률을 수치로 주장하지 않습니다.
- 제공된 기존 v1.24 사고의 재분석으로 로그 선택과 요약 보정은 확인했지만, 최초 예외 보존 변경은 새 v0.2.57 DLL을 설치한 뒤 발생한 실사고로 현장 검증이 한 번 더 필요합니다.
- 플러그인, Helper, 분석기와 WinUI가 함께 바뀌므로 이전 릴리즈 파일과 섞지 말고 zip 전체를 업데이트해 주세요.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `62/62` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `58/58` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.57.zip`, `--no-pdb`): 성공 (`87,652,779` bytes, 523 entries, PDB 0개).
- Release gate: `OK` (핵심 PE/Windows App SDK x64, 현재 빌드 해시 일치).
- 실사고 품질 코퍼스: `SKIPPED (not measured)` — 코퍼스 미제공.
- SHA-256: `543F522018031FD078CAF78D459897FAB7228E7AD3CB190F76A1A83BFD5F3B83`.

## v0.2.56 (2026-07-20)

### 한눈에 보기
- 이번 릴리즈는 **정상 종료 오탐 경합, CTD 근거 신뢰도, 런타임 비용과 배포 검증을 함께 보강**합니다.
- 크래시 버킷을 심볼 문자열 대신 모듈명과 RVA로 계산하는 `CTD2` 형식으로 전환하고, 같은 덤프 재분석이 history 통계를 부풀리지 않도록 했습니다.
- Crash Logger 로그가 여러 개 가까운 시각에 존재하면 모호한 페어링으로 표시하고 해당 단서의 신뢰도와 후보 가중치를 낮춥니다.

### 수정
- **정상 종료 분석 경합 제거** — headless 분석기 프로세스를 Helper가 추적하고, 최종 `exit_code=0`이면 프로세스를 종료한 뒤 CTD 파생 산출물을 정리해 늦은 summary 재생성을 막습니다.
- **Crash history 멱등성** — dump 파일명을 대소문자 비구분 키로 사용해 동일 덤프 재분석 결과를 갱신하고, 현재 덤프는 반복 근거 계산에서 제외합니다.
- **Canonical crash bucket v2** — 예외 코드, fault module+RVA, 선택된 callstack의 module+RVA를 해시해 심볼 서버 상태나 함수명 표현 차이에 덜 민감한 `CTD2-*` 키를 생성합니다.
- **Crash Logger 페어링 품질** — 선택 로그의 시간차, 차순위 시간차, 유효 후보 수와 근접 경쟁 로그 수를 summary/report에 기록합니다. 2초 이내 경쟁 로그가 있으면 독립 stack suspect 재정렬과 High 승격을 막습니다.
- **플러그인 핫패스 경량화** — first-chance 예외 rate limit을 모듈 경로 확인보다 먼저 수행하고 고정 버퍼를 사용합니다. heartbeat와 별개인 모듈/스레드 lifecycle 열거 주기를 1초로 낮춥니다.
- **릴리스 hard gate 강화** — 버전명, PDB 부재, 핵심 PE x64, 현재 빌드와 ZIP 내부 파일의 SHA-256 일치, self-contained Windows App SDK 런타임 파일 포함을 검증합니다.
- **실사고 품질 게이트 연결** — 검토 완료 코퍼스와 모든 임계값이 설정된 경우에만 정확도 기준을 강제하며, 코퍼스가 없으면 통과로 오인하지 않도록 `SKIPPED (not measured)`로 표시합니다.
- **WinUI 안내 정리** — v0.2.52+ self-contained 배포와 v0.2.53+ launcher/app 폴더 구조를 README, Beta, Nexus 안내에 맞게 통일했습니다.

### 주의사항
- 새 버킷 키는 `CTD2-` 접두사를 사용하므로 기존 `CTD-` history 그룹과 자동으로 합쳐지지 않습니다.
- 실제 CTD 원인 적중률은 `triage.ground_truth_mod`가 채워진 검토 완료 실사고 코퍼스가 필요합니다. 이번 릴리즈는 합성 테스트만으로 정확도 백분율을 주장하지 않습니다.
- v0.2.52+에서는 .NET Desktop Runtime 8 또는 Windows App Runtime 1.8을 별도로 설치할 필요가 없습니다. 런타임 설치 창이 뜨면 기존 `SkyrimDiagWinUI` 폴더를 제거하고 zip 전체를 다시 설치해 주세요.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `62/62` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `58/58` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.56.zip`, `--no-pdb`): 성공 (`87,647,479` bytes, 523 entries, PDB 0개).
- Release gate: `OK` (핵심 PE/Windows App SDK x64, 현재 빌드 해시 일치).
- 실사고 품질 코퍼스: `SKIPPED (not measured)` — 코퍼스 미제공.
- SHA-256: `E10D82377C43CD9ED3E0CE36730B73983804C3C4B8A0AA779DF74531CB59C072`.

## v0.2.55 (2026-07-18)

### 한눈에 보기
- 이번 릴리즈는 **정상 게임 종료를 CTD로 오인하던 문제를 수정한 hotfix**입니다.
- SKSE DLL의 first-chance 접근 위반이 기록되더라도 게임 프로세스의 최종 `exit_code=0`이면 처리된 예외로 판정합니다.
- 정상 종료가 확정되면 CTD 분석, 지연 뷰어, 덤프와 파생 리포트가 사용자에게 실제 CTD처럼 노출되지 않습니다.

### 수정
- **종료 코드 우선 판정** — 예외 코드의 강도와 관계없이 최종 프로세스 종료 코드가 0이면 정상 종료로 확정하고, 비정상 종료(`exit_code!=0`)의 기존 CTD 처리는 유지합니다.
- **strong-exception 우회 제거** — 공유 메모리에 접근 위반이 남아 있어도 정상 종료를 CTD로 되돌리거나 지연 뷰어를 실행하지 않습니다.
- **오탐 산출물 정리** — 진행 중인 headless 분석과 ETW를 중단하고 dump, report, summary, blackbox, WCT, PluginScan, incident manifest를 정리합니다.
- **보존 옵션 정합성** — `PreserveFilteredCrashDumps=1`에서는 원본 dump만 남기고 파생 CTD 산출물과 capture/viewer latch는 제거합니다.
- **회귀 테스트** — `0xC0000005 + InMenu + exit_code=0` 사례와 strong shared-memory crash evidence가 있는 실제 프로세스 종료 경로를 추가했습니다.

### 주의사항
- 외부 크래시 핸들러가 실제 치명적 CTD의 프로세스 종료 코드를 강제로 0으로 바꾸는 매우 드문 환경에서는 해당 사고가 정상 종료로 필터링될 수 있습니다.
- 필터링된 원본 dump를 조사 목적으로 남기려면 `SkyrimDiagHelper.ini`에서 `PreserveFilteredCrashDumps=1`을 사용해야 합니다.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `61/61` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `57/57` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.55.zip`, `--no-pdb`): 성공 (`87,614,201` bytes, 523 entries, PDB 0개).
- Release gate: `OK`.
- SHA-256: `C58113A22589947F08504F28B2199031945E746C26251B1D20FC615CE0BB010C`.

## v0.2.54 (2026-07-13)

### 한눈에 보기
- 이번 릴리즈는 **CTD 원인 과단정 방지와 크래시 캡처 정합성 보강** 릴리즈입니다.
- 플러그인과 Helper 사이의 크래시 컨텍스트를 원자적으로 커밋해, 기록 도중의 예외 정보가 덤프에 섞이는 가능성을 줄였습니다.
- 알려진 크래시 서명은 기본적으로 "발생 메커니즘"을 설명하고, 별도의 후보 합의 결과가 실제 근본 원인 후보를 판단하도록 역할을 분리했습니다.
- Crash Logger의 원시 프레임은 보존하되 시스템 DLL, 게임 실행 파일, 훅 프레임워크 같은 비행동 신호가 유력 원인으로 승격되지 않도록 했습니다.

### 수정
- **Crash capture: 커밋 시퀀스 도입** — `crash_seq` seqlock 프로토콜로 플러그인 기록과 Helper 읽기를 동기화하고, 동일한 안정 스냅샷으로 blackbox/exception stream을 생성합니다.
- **Crash capture: 복구·종료 경합 보정** — 복구된 first-chance 예외는 자신이 기록한 시퀀스만 해제할 수 있으며, Helper는 프로세스 종료 전에 대기 중인 crash event를 먼저 처리합니다.
- **Signature: 메커니즘/근본 원인 분리** — 서명 schema를 엄격히 검증하고 `scope`, `mechanism`, `match_confidence`를 출력합니다. `D6DDDA_VRAM`은 SkyrimSE 1.5.97.0의 정확한 접근 위반 패턴인 `D6DDDA_1597_AV`로 좁혔습니다.
- **Candidate consensus: 예외 스레드 우선** — 정상 stackwalk와 fallback scan 모두 예외 스레드를 우선하고, 낮은 품질의 stack 및 capture-quality 신호가 후보 신뢰도를 과도하게 올리지 못하도록 했습니다.
- **Candidate identity/history: 키 충돌 방지** — 후보 키가 구분자와 Unicode를 보존하며, 모호한 v1 history key가 다른 후보를 잘못 boost하지 않도록 history schema v2를 사용합니다.
- **Crash Logger: 로그 페어링 강화** — dump/log artifact 종류를 구분하고 최대 시간 창을 120초로 제한하며, 이름 규칙보다 실제 시간 차이를 먼저 비교합니다.
- **Crash Logger/WinUI: 원시 관측과 행동 후보 분리** — 세 frame field별 eligibility를 summary에 기록하고, WinUI와 권장 조치가 같은 행동 가능성 판정을 사용합니다.
- **품질 게이트: 실사용 표시 순서 측정** — 중복 incident와 충돌 라벨을 fail-closed하고 top-1 accuracy, top-3 recall, High-confidence precision, abstention rate를 분리해 측정합니다.
- **테스트: release build 검증 실효성 강화** — RelWithDebInfo에서도 assertion을 활성화하고, Crash Logger 실제 파일/타임스탬프 통합 테스트와 새 summary schema 회귀 검사를 추가했습니다.
- **개발 도구 정리** — 더 이상 사용하지 않는 Vibekit 스크립트, 에이전트 안내, 전용 CI/테스트를 제거하고 현재 빌드·패키징 계약에 맞췄습니다.

### 주의사항
- 공유 메모리 프로토콜이 v3으로 올라갔으므로 `SkyrimDiag.dll`과 `SkyrimDiagHelper.exe`를 서로 다른 릴리즈에서 섞지 말고 zip 전체를 함께 업데이트해야 합니다.
- 서명의 High confidence는 해당 패턴의 일치 신뢰도이며, 특정 모드·에셋이 근본 원인이라는 자동 확정을 의미하지 않습니다.
- 실제 CTD 원인 적중률은 검토자가 `triage.ground_truth_mod`를 채운 실사고 코퍼스로 별도 측정해야 하며, 합성 테스트 결과를 정확도 백분율로 사용하지 않습니다.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `61/61` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `57/57` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.54.zip`, `--no-pdb`): 성공 (`87,612,303` bytes, 523 entries, PDB 0개).
- Package content check: protocol v3 Plugin/Helper와 `D6DDDA_1597_AV` 서명 포함 확인.
- Release gate: `OK`.
- SHA-256: `50090AC11C6E2B99C46ACEBFDD0AA34AA48A22F60F3DB8604F781C9CEF76AEC4`.

## v0.2.53 (2026-05-08)

### 한눈에 보기
- 이번 릴리즈는 **WinUI self-contained 폴더 정리 hotfix**입니다.
- `v0.2.52`에서 런타임 오류를 막기 위해 self-contained 파일을 모두 포함하면서 `SkyrimDiagWinUI` 폴더가 너무 복잡해진 문제를 정리했습니다.
- 이제 사용자는 `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`만 찾으면 되고, 많은 .NET/Windows App SDK 런타임 파일은 `SKSE/Plugins/SkyrimDiagWinUI/app/` 아래로 모입니다.

### 수정
- **WinUI launcher 추가** — Helper가 기존처럼 `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`를 실행하면, 작은 네이티브 런처가 `app\SkyrimDiagDumpToolWinUI.exe`를 실행하고 종료까지 기다립니다.
- **Package layout 정리** — 실제 WinUI self-contained 앱, `SkyrimDiagDumpToolNative.dll`, analyzer data 파일을 `SkyrimDiagWinUI/app/` 아래에 배치합니다.
- **Dump discovery 보정** — 실제 WinUI 앱이 `app/` 하위에서 실행되어도 `SkyrimDiagHelper.ini`와 MO2 overwrite 출력 위치를 올바르게 찾도록 경로 추론을 보강했습니다.
- **Helper diagnostics 보정** — WinUI가 즉시 종료될 때 안내하는 로그 경로를 새 launcher/app 레이아웃에 맞췄습니다.

### 테스트
- Packaging/WinUI dump discovery target tests: 실패 확인 후 통과.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI self-contained publish: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.53.zip`, `--no-pdb`): 성공 (`87,689,734` bytes).
- Package layout check: `SkyrimDiagWinUI` 최상위 1개 파일, `app/` 아래 511개 파일.
- Release gate: `OK`.
- WinUI launcher startup smoke: zip 추출본에서 launcher가 5초 이상 정상 실행 상태 유지.
- Windows helper runtime smoke 3종: 통과.

## v0.2.52 (2026-05-08)

### 한눈에 보기
- 이번 릴리즈는 **WinUI 뷰어 런타임 설치 오류 hotfix**입니다.
- `SkyrimDiagDumpToolWinUI.exe`가 Windows App Runtime 1.8/MSIX 설치 상태에 민감하게 실패하던 배포 방식을 self-contained publish로 바꿨습니다.
- 릴리즈 zip 크기는 커지지만, 사용자는 WinUI 뷰어 실행을 위해 .NET Desktop Runtime 8 또는 Windows App Runtime 1.8을 별도로 설치할 필요가 없습니다.

### 수정
- **WinUI 배포: self-contained 전환** — `dotnet publish --self-contained true`와 `WindowsAppSDKSelfContained=true`를 사용해 WinUI 실행 파일 옆에 .NET/Windows App SDK 런타임 파일을 함께 배포합니다.
- **Release gate: zip 크기 기준 갱신** — self-contained WinUI 포함으로 정상 zip 크기가 수십 MB까지 커질 수 있어 hard gate를 100MB로 조정했습니다.
- **문서: 런타임 안내 보정** — v0.2.52+ 릴리즈에서는 별도 Windows App Runtime 1.8 설치가 필요 없다는 점과, v0.2.49~v0.2.51의 기존 framework-dependent 동작을 명확히 적었습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI self-contained publish: 성공 (`build-winui`, 330개 파일 / 약 213MB uncompressed).
- Packaging(`dist/Tullius_ctd_loger_v0.2.52.zip`, `--no-pdb`): 성공 (`87,572,243` bytes).
- Release gate: `OK`.
- WinUI startup smoke: `SkyrimDiagDumpToolWinUI.exe`가 5초 이상 정상 실행 상태 유지.

## v0.2.51 (2026-05-04)

### 한눈에 보기
- 이번 릴리즈는 **CrashLoggerSSE v1.21/v1.22 객체 introspection 호환성 보강**입니다.
- Crash Logger가 새로 출력하는 `SpellItem`, `EffectSetting`, `BGSLocation`, `NavMesh` 요약 라인에서도 원인 후보 ESP/FormID와 객체 타입/이름을 유지하도록 파서를 보강했습니다.
- 기존 `RDI: (Character*) ...` 형식은 그대로 유지하고, `RDX: RE::SpellItem "..." [0x...] (Mod.esp)` 같은 새 요약 포맷만 fallback으로 처리합니다.

### 수정
- **CrashLogger parser: simplified introspection 지원** — `POSSIBLE RELEVANT OBJECTS` 안의 `RE::SpellItem`, `RE::EffectSetting`, `RE::BGSLocation`, `RE::NavMesh` 라인에서 타입, 표시 이름, FormID, plugin 파일명을 함께 추출합니다.
- **Regression tests: v1.21/v1.22 객체 라인 가드 추가** — 최신 CrashLoggerSSE의 spell/effect/location/navmesh 예시 라인을 파서 테스트에 추가해 이후 포맷 회귀를 잡도록 했습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.51.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.50 (2026-04-28)

### 한눈에 보기
- 이번 릴리즈는 **MO2 활성 프로필 기준의 provider/slot 진단 정확도 보정**입니다.
- 활성 profile의 `modlist.txt`를 읽은 경우, 비활성화된 모드가 loose-file provider 후보처럼 보이지 않도록 했습니다.
- ESL / full plugin 슬롯 경고는 활성 플러그인만 세고, full plugin 판정이 불확실한 항목은 고신뢰 슬롯 한계 경고에서 제외하도록 조정했습니다.
- 프로젝트를 `G:\skyrim project\Tullius_ctd_loger`로 옮긴 뒤에도 릴리즈 게이트와 외부 build tree 테스트가 현재 repo root를 올바르게 잡도록 보강했습니다.

### 수정
- **MO2 provider hint: 비활성 모드 제외** — 활성 profile modlist를 정상적으로 읽은 경우 provider 검색을 활성 모드 범위로 제한해, 꺼져 있는 모드가 리소스 제공 후보처럼 표시되는 오탐을 줄였습니다.
- **Plugin rules: 활성 슬롯 기준 보정** — `esl_count_gte`는 활성 ESL만 세도록 바꾸고, 새 `full_plugin_count_gte` 조건은 활성 full plugin 중 슬롯 타입을 알고 있는 항목만 세도록 추가했습니다.
- **Release gate: 이동 후 경로 안정화** — `verify_release_gate.sh`가 예전 WSL checkout / Windows mirror 경로 대신 스크립트 위치에서 기본 repo root를 계산하도록 변경했습니다.
- **Tests: 외부 build tree 안정화** — `skydiag_candidate_consensus_tests`가 repo 밖 build tree에서도 source root를 찾도록 CTest 환경을 보강했습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.50.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.49 (2026-04-02)

### 한눈에 보기
- 이번 정식 릴리즈는 **non-system DLL CTD에서 단일 DLL 과단정을 더 줄이는 정확도 보정**입니다.
- 이제 non-system DLL CTD도 `actionable_candidates` 합의 경로에 들어가고, raw fault DLL 하나만으로 후보가 고정되지 않도록 조정했습니다.
- `Crash Logger frame + 같은 덤프 stack`만 있는 경우는 더 이상 독립 교차검증처럼 취급하지 않고, `fault-location cluster`로 낮춰 보여줍니다.

### 수정
- **Engine: non-system DLL candidate consensus 확장** — EXE/system/hook/hang 케이스에만 국한되던 `actionable_candidates` 합의 경로를 non-system DLL CTD에도 열어, `Crash Logger frame`, `stack`, `history`, `resource` 같은 신호를 함께 비교하도록 확장.
- **Engine: weak fault-location cluster 강등** — `Crash Logger frame + 같은 덤프 stack` 정도만 있는 후보는 `cross_validated`처럼 승격하지 않고, low/cautious path로 내려 summary/evidence/recommendation이 피해 위치 가능성을 더 정직하게 노출하도록 조정.
- **Output: phrasing 정렬** — non-system DLL CTD에서 `유력 후보`처럼 읽히던 wording을 `fault-location 단서`, `주변 probable DLL 비교` 쪽으로 옮겨, raw crash site와 최종 해석을 더 분리해서 읽을 수 있게 함.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.49.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.49-rc1 (2026-03-31)

### 한눈에 보기
- 이번 프리릴리즈는 **non-system DLL CTD에서 단일 DLL 과단정을 더 줄이는 정확도 보정**입니다.
- 이제 non-system DLL CTD도 `actionable_candidates` 합의 경로에 들어가고, raw fault DLL 하나만으로 후보가 고정되지 않도록 조정했습니다.
- `Crash Logger frame + 같은 덤프 stack`만 있는 경우는 더 이상 독립 교차검증처럼 취급하지 않고, `fault-location cluster`로 낮춰 보여줍니다.

### 수정
- **Engine: non-system DLL candidate consensus 확장** — EXE/system/hook/hang 케이스에만 국한되던 `actionable_candidates` 합의 경로를 non-system DLL CTD에도 열어, `Crash Logger frame`, `stack`, `history`, `resource` 같은 신호를 함께 비교하도록 확장.
- **Engine: weak fault-location cluster 강등** — `Crash Logger frame + 같은 덤프 stack` 정도만 있는 후보는 `cross_validated`처럼 승격하지 않고, low/cautious path로 내려 summary/evidence/recommendation이 피해 위치 가능성을 더 정직하게 노출하도록 조정.
- **Output: phrasing 정렬** — non-system DLL CTD에서 `유력 후보`처럼 읽히던 wording을 `fault-location 단서`, `주변 probable DLL 비교` 쪽으로 옮겨, raw crash site와 최종 해석을 더 분리해서 읽을 수 있게 함.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.49-rc1.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.48 (2026-03-31)

### 한눈에 보기
- 이번 버전은 **Crash Logger 단서를 Tullius 결론과 더 정확히 맞추는 후속 패치**입니다.
- Crash Logger의 `CALL STACK ([P]robable / [S]tack scan)` 형식을 제대로 읽어, `[P]` 체인을 실제 후보 근거로 반영합니다.
- non-system DLL에서 direct fault가 잡혀도, 독립 근거가 부족하면 바로 `유력 후보 / 높음`으로 단정하지 않도록 완화했습니다.
- mod author에게 바로 보고하라는 안내도 `cross_validated`처럼 교차검증된 경우에만 유지합니다.

### 수정
- **Crash Logger parser: mixed call stack 형식 지원** — `PROBABLE CALL STACK:`뿐 아니라 `CALL STACK ([P]robable / [S]tack scan):` 헤더도 인식하고, 혼합 형식에서는 `[P]` 행만 probable call stack으로 수집하도록 수정.
- **Summary: non-system DLL 과단정 완화** — direct fault DLL이 있어도 `frame only`, `reference clue`, `related`, `conflicting`, `cross_validated` 상태를 구분해 요약 문장을 다르게 쓰고, 피해 위치(victim location) 가능성을 더 정직하게 노출.
- **Recommendations: DLL guidance 조정** — 독립 신호 합의가 없는 경우에는 DLL을 바로 근본 원인으로 단정하지 말라는 안내를 우선하고, mod author 보고 권고는 fault-module candidate가 교차검증된 경우에만 노출.

### 테스트
- Crash Logger parser 테스트에 mixed `[P]/[S]` call stack fixture를 추가.
- output snapshot / analysis engine runtime 테스트에 `no second independent signal`, `victim location`, `fault-location evidence only` 같은 비과장 계약을 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.47 (2026-03-31)

### 한눈에 보기
- 이번 버전은 **실사용 피드백으로 확인된 환경 탐지 오경고를 줄이는 후속 패치**입니다.
- `msdia140.dll`이 게임의 `SKSE\Plugins`에만 있어도 분석기가 실제로 찾고 사용할 수 있게 했습니다.
- MO2 환경에서 `plugins.txt`를 놓쳐 `Could not find plugins.txt`가 뜨던 케이스를 더 잘 따라가도록 보강했습니다.
- 플러그인 헤더를 읽지 못한 경우에는 `ESP_FULL_SLOT_NEAR_LIMIT`를 고신뢰 경고처럼 띄우지 않도록 조정했습니다.

### 수정
- **Engine: bundled `msdia140.dll` 탐지 보강** — 분석기 프로세스 DLL 검색 경로에 없더라도, 게임 설치의 `Data\SKSE\Plugins\msdia140.dll`을 직접 찾아 로드할 수 있게 조정.
- **Helper: MO2 profile 탐지 보강** — `usvfs_x64.dll`/`uvsfs64.dll` 모듈 실제 경로를 이용해 `ModOrganizer.ini`와 활성 profile의 `plugins.txt`를 찾는 fallback을 추가.
- **Preflight: non-ESL 슬롯 경고 false positive 완화** — 플러그인 헤더를 읽지 못한 항목은 `slot_type_known`으로 분리하고, 슬롯 분류가 불완전할 때는 `254 슬롯 근접` 경고를 대략적 검사로 낮춰 표시.

### 테스트
- plugin scanner 가드 테스트에 `slot_type_known`, MO2 module-path fallback, plugin stream 계약 검증을 추가.
- preflight 가드 테스트에 `slot-limit check is approximate` 계약을 추가.
- analysis engine runtime 테스트에 bundled `msdia140.dll` 탐지 소스 계약을 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.46 (2026-03-30)

### 한눈에 보기
- 이번 버전은 **WinUI에서 바로 보이는 작은 불편과 지원 혼선을 줄이는 유지보수 업데이트**입니다.
- `Raw Data` 탭의 긴 텍스트를 더 직접적으로 스크롤해서 볼 수 있게 했습니다.
- `Triage` 탭에서 `Evidence` 패널을 접고 펼칠 때 페이지 폭이 흔들리는 현상을 줄였습니다.
- `address_db` 로딩 실패 메시지를 더 구체적으로 나눠, 파일 누락인지 게임 버전 미지원인지 바로 구분할 수 있게 했습니다.

### 수정
- **WinUI: Raw Data 텍스트 박스 스크롤바 명시** — `WCT JSON`은 가로/세로 스크롤을 모두 직접 사용할 수 있게 하고, `Report`는 세로 스크롤을 안정적으로 노출해 긴 출력 확인이 쉬워지도록 조정.
- **WinUI: Triage 레이아웃 폭 흔들림 완화** — 루트 스크롤 영역이 세로 스크롤바 폭을 항상 예약하도록 바꿔 `Evidence` expander 확장/축소 시 본문 폭이 변하는 현상을 줄임.
- **Engine: address_db 진단 세분화** — `address_db/skyrimse_functions.json` 실패를 단일 문구로 뭉뚱그리지 않고, `파일 없음`과 `현재 game_version 항목 없음`을 구분해서 보고하도록 개선.

### 테스트
- WinUI XAML 가드 테스트에 `Raw Data` 스크롤바 계약과 루트 스크롤바 폭 고정 계약을 추가.
- AddressResolver 런타임 테스트에 load status 분기와 `missing file / missing game version` 구분 케이스를 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.45 (2026-03-25)

### 한눈에 보기
- 이번 버전은 **CTD 원인 후보를 더 쉽게 읽고 더 덜 헷갈리게 보여주는 업데이트**입니다.
- Crash Logger가 같이 있는 경우, Tullius가 **Crash Logger가 가리키는 DLL 후보를 전보다 더 앞에, 더 직접적으로 보여줍니다.**
- Crash Logger가 없어도, Tullius 단독 callstack 분석 결과를 **약한 추정과 구분해서** 읽기 쉽게 정리했습니다.
- freeze / hang 진단은 **근거가 부족하면 무리하게 단정하지 않고**, 합의된 신호가 있을 때만 더 강하게 보여주도록 조정했습니다.
- 공유 텍스트와 텍스트 리포트도 정리해서, **왜 이 후보를 의심하는지**를 예전보다 바로 이해하기 쉬워졌습니다.

### 추가
- **CTD: Crash Logger frame-first 해석 경로 강화** — direct fault DLL, 첫 actionable probable frame, same-DLL streak, C++ exception module을 CTD 후보 승격의 핵심 신호로 반영. EXE/system victim 크래시에서도 Crash Logger가 강하게 가리키는 DLL 후보를 summary/report/WinUI/share text에서 먼저 보여주도록 개선.
- **CTD: Tullius 단독 callstack 해석 경로 보강** — Crash Logger가 없는 상태에서도 강한 stackwalk-only 후보를 별도 경로로 드러내고, 약한 stack-scan 단서와 구분해 표시하도록 정리.
- **Capture quality: richer crash dump profile 도입** — crash / crash recapture profile에 `process_thread_data`, `full_memory_info`, `module_headers`, `indirect_memory`, `ignore_inaccessible_memory`를 배선하고, callback-shaped dump bootstrap을 추가해 더 나은 CTD 해석 입력을 확보.
- **Freeze/PSS: snapshot + WCT 합의 품질 노출** — freeze snapshot flags를 `VA_SPACE` / `SECTION_INFORMATION`까지 확대하고, WCT 2회 캡처 기반 `cycle_consensus`, `consistent_loading_signal`, `capture_passes` 메타데이터를 summary/report에 기록.

### 수정
- **CTD: ambiguous candidate 노이즈 완화** — `frame`이 이미 합의된 후보를 `object-ref/history` 보조 신호가 불필요하게 `conflicting`으로 끌어내리던 경로를 줄이고, `frame + first-chance`, `frame + history`, `frame + near resource provider` 같은 다중 신호를 더 자연스럽게 보여주도록 조정.
- **공유/리포트: 해석 경로를 직접 노출** — WinUI 공유 텍스트와 텍스트 리포트에 `CrashLogger reading path`, `Next action`, `capture quality`, `freeze support_quality`를 직접 표시해 사용자가 왜 그런 결론이 나왔는지 바로 볼 수 있도록 정리.
- **Freeze: legacy WCT 샘플 보수 해석 유지** — 새 consensus 메타데이터가 없는 과거 hang dump는 `freeze_ambiguous` / `live_process` 수준으로 안전하게 내려가도록 재검증.

### 테스트
- Crash Logger 최소 excerpt fixture 6종과 share text fixture 5종을 추가해 `parser -> candidate -> summary -> WinUI/share text` 회귀를 고정.
- capture profile / dump writer / incident manifest / freeze consensus 가드 테스트를 확장.
- Linux release CI에 `.NET 8 SDK` setup을 추가하고, share text fixture runner가 non-WSL Linux 경로를 직접 사용하도록 조정.
- Linux 전체 테스트 `57/57` 통과, Windows native build / WinUI build / packaging / release gate 확인.

## v0.2.44 (2026-03-24)

### 수정
- **Helper: machine-code-aware dump capture 보강** — 기본 crash profile과 crash recapture profile에서 `MiniDumpWithCodeSegs`를 함께 요청하도록 변경. 외부 reverse-engineering/disassembly 도구가 dump 안에서 기계어 바이트를 찾지 못해 `not found machine code`로 실패하던 사례를 완화.
- **Dump metadata: code segment 포함 여부 노출** — incident manifest, summary JSON, report text에 `include_code_segments` / `CaptureProfileCodeSegments`를 기록해 실제 캡처 프로필을 사후 확인할 수 있도록 정리.
- **문서/배포 INI: DumpMode=1 설명 보정** — 배포용 `SkyrimDiagHelper.ini` 주석을 현재 기본 프로필(`WithThreadInfo+HandleData+UnloadedModules+CodeSegs`)에 맞게 갱신.

### 테스트
- dump profile/source guard 테스트에 code-segment 캡처 계약 검증 추가.
- incident manifest / output snapshot 테스트에 code-segment 메타데이터 출력 검증 추가.
- Linux: `ctest --test-dir build-linux-red --output-on-failure` 통과(`55/55`).

## v0.2.43 (2026-03-23)

### 수정
- **Helper: blank `OutputDir` 기본 출력 하위 폴더 적용** — `OutputDir=`를 비워 두면 기본 출력 위치 바로 아래가 아니라 `Tullius Ctd Logs` 하위 폴더를 사용하도록 변경. MO2 `overwrite`가 빠르게 지저분해지는 문제를 완화.
- **WinUI: 새 기본 출력 폴더 자동 발견** — blank `OutputDir` 환경에서 `Tullius Ctd Logs` 하위 폴더를 우선 스캔하고, 기존 legacy 기본 위치도 함께 찾아서 업데이트 직후에도 기존 dump를 계속 발견할 수 있도록 조정.
- **문서/배포 INI: `OutputDir` 사용법 명확화** — 따옴표 불필요, 상대경로 허용, blank 값의 의미를 README/한글 문서/Nexus 설명/배포용 ini에 맞춰 정리.

### 리팩터링
- Helper: 기본 출력 경로 계산 헬퍼를 정리하고, 더 이상 쓰지 않는 중복 기본 경로 처리 코드를 제거.

### 테스트
- helper 설정 가드 테스트에 기본 `Tullius Ctd Logs` 계약 검증 추가.
- WinUI 자동 dump 발견 가드 테스트에 새 기본 출력 하위 폴더 및 legacy fallback 검증 추가.

## v0.2.42 (2026-03-05)

### 추가
- **DumpTool: CrashLogger ESP/ESM 오브젝트 참조 파싱** — CrashLogger의 POSSIBLE RELEVANT OBJECTS / REGISTERS 섹션에서 크래시 시점에 처리 중이던 게임 오브젝트의 소속 ESP/ESM을 파싱. 게임 EXE 내부 크래시에서 DLL 기반 용의자를 특정할 수 없을 때 "어떤 모드의 오브젝트를 처리 중이었는지" 증거와 권장 조치를 제공.
- **DumpTool: CrashLogger FormID 파싱** — `[0xFEAD081B]` 형태의 FormID를 ESP 참조와 함께 추출. JSON/텍스트 출력, 근거(Evidence), 권장사항, WinUI Quick Summary·공유·클립보드 텍스트에 FormID 표시. xEdit에서 문제 오브젝트를 바로 찾을 수 있는 핵심 정보 제공.
- **Helper: NGIO 잔디 캐싱 모드 자동 감지** — Skyrim 루트에 `PrecacheGrass.txt`가 있으면 크래시/행 감지를 모두 억제하고 경량 대기 루프로 전환. MO2 GrassPrecacher의 자동 재시작 사이클이 Helper 팝업에 의해 방해받지 않음.
- Helper: `SuppressDuringGrassCaching` INI 옵션 추가 (기본값 1). 0으로 설정 시 잔디 캐싱 모드 감지 비활성화.
- DumpTool: `IsSystemishModule` D3D/DXGI/OpenGL/디버깅 DLL 13종 추가 — 그래픽 드라이버 DLL이 용의자로 잘못 표시되는 문제 완화.
- DumpTool: `TroubleshootingGuideDatabase` 클래스 추출 — 트러블슈팅 가이드 매칭 로직을 독립 클래스로 분리, 재사용 가능.
- DumpTool: 리소스 로그 보존 상한 80→120 확대.
- DumpTool: CrashLogger 타임스탬프 파싱 함수 (`TryExtractCompactTimestampFromStem`, `TryExtractDashedTimestampFromStem`) 추가 + 검증 테스트 13개.

### UI
- **WinUI: Glassmorphism + Gradient 비주얼 향상** — AcrylicBrush 반투명 카드 배경, cyan→purple 그라데이션 악센트 바/보더, KPI 카드 상단 그라데이션 바, Suspects 좌측 그라데이션 스트라이프, 섹션 아이콘(FontIcon) 추가, 2컬럼 글로우 디바이더, ANALYZE NOW 버튼 그라데이션 적용.

### 수정
- **WinUI: Quick Summary 카드에 CrashLogger ESP/ESM 우선 표시** — 기존에 DLL 스택 스캔만 표시하던 Quick Summary 카드, 후보 목록, 공유 텍스트를 CrashLogger ESP 참조 우선으로 개편. 요약 문장과 UI가 일치하도록 수정.
- DumpTool: CrashLogger 시간 매칭 창 30분→5분 축소 — 무관한 과거 로그 매칭 방지.

### 리팩터링
- **코드 중복 대폭 제거**: `ConfidenceText` 5곳→I18nCore.h 1곳, `MakeKernelName` 2곳→SkyrimDiagProtocol.h, `Hex32`/`Hex64` 2곳→HexFormat.h.
- DumpTool: 스코어링 매직넘버 14개를 명명 상수로 전환 (`kWeightDepth0`, `kHighConfMinScore` 등).
- DumpTool: `CrashLoggerRankBonus` 매직넘버 5개 상수화.
- DumpTool: `ScopedHistoryFileLock` 디렉토리 기반→Windows Named Mutex 전환 — 프로세스 크래시 시 잠금 자동 해제.
- DumpTool: `AnalyzeDump()` 550줄 → 11개 서브함수 분할 (`LoadSupportDatabases`, `IntegrateCrashLogger`, `RunStackwalk` 등).
- DumpTool: `BuildEvidenceItems`/`WriteOutputs` 분리, `isActionableSuspect` 중복 제거, `CrashLoggerParseCore.h` → `.h/.cpp` 분리.
- DumpTool: 진단 로깅 인프라 — `AnalysisResult.diagnostics` 벡터로 데이터 로드 실패, CrashLogger 통합 에러, 스택워크 폴백 등 8개 경로에서 best-effort 실패 메시지 수집. JSON/텍스트 출력 + WinUI 표시.
- Helper: Win32 HANDLE RAII 래퍼 `UniqueHandle` 도입 — `CreateFileW`/`CreateMutexW`/`OpenProcess` 등 수동 `CloseHandle` 6곳 제거.
- WinUI: **MVVM 패턴 적용** — `MainWindow.xaml.cs` 1053→465줄(56% 감소). 상태/컬렉션/텍스트 빌더를 `MainWindowViewModel.cs`로 분리. 코드비하인드는 UI 바인딩·이벤트 핸들러만 담당.
- Plugin: 워치독 스레드 `std::thread::detach()` → `std::jthread` + `stop_token` — DLL 언로드 시 안전한 종료.

### 인프라
- `.clang-tidy` 정적 분석 설정 — bugprone/performance/modernize 규칙.
- CI: ASan+UBSan 빌드 잡 추가 (`linux-asan`).
- libFuzzer 퍼징 하네스 2개 추가 (`fuzz_crashlogger_parser`, `fuzz_wct_parser`) + 시드 코퍼스.

### 테스트
- CrashLogger 타임스탬프 파싱 테스트 13개 추가 (Compact/Dashed 포맷, 유효성 검증, 엣지케이스).
- CrashLogger ESP/ESM 오브젝트 참조 파싱 테스트 16개 추가 (바닐라/CC 필터, Modified by 스킵, 유니코드 이름, 스코어링, 집계, malformed 입력 방어).
- CrashLogger FormID 파싱 테스트 10개 추가 (`ExtractFormIdBefore`, `ExtractEspRefsFromLine`, 전파/집계 검증).
- WCT JSON 파싱 테스트 11개 추가 (빈 입력, 사이클 우선순위, maxN 제한, capture 파싱).
- MO2 경로 추론 테스트 10개 추가 (대소문자, 역슬래시, 빈 입력 방어).
- 진단 로깅 가드 테스트 추가 (소스 파일 내 diagnostics 인프라 존재 검증).
- 총 테스트 수: 47개 (기존 26개 → 47개).

## v0.2.41 (2026-02-28)

### 추가
- **Plugin: 크래시 예외 필터를 블랙리스트 방식으로 전환** — 기존 화이트리스트(15개 코드) 대신 블랙리스트(5개 무해 코드 제외)를 적용. `EXCEPTION_NONCONTINUABLE_EXCEPTION`, `STATUS_FATAL_APP_EXIT`, 모드 커스텀 예외 등 이전에 누락되던 크래시를 자동 감지.
- **Plugin: 모드 메뉴 이름 자동 표시** — MenuOpen/Close 이벤트 payload에 메뉴 이름 UTF-8 문자열을 인라인 저장. 모든 모드 메뉴가 해시 대신 실제 이름(`SKI_WidgetMenu`, `TrueHUD` 등)으로 표시됨 (구버전 덤프 하위 호환).
- **DumpTool: 이벤트 로그 가독성 개선** — `FormatEventDetail` 구현으로 PerfHitch, MenuOpen/Close(FNV-1a 해시 → 알려진 메뉴 이름 역해석), Heartbeat/CellChange에 사람이 읽을 수 있는 요약 텍스트 자동 생성. 프리징 직전 10초 이내 이벤트 컨텍스트 요약을 Evidence에 추가.
- **DumpTool: 크래시 히스토리 상관 분석** — 동일 `crash_bucket_key` 반복 발생 시 Evidence에 "반복 크래시 패턴" 표시 + Summary JSON에 `history_correlation` 필드 출력.
- **DumpTool: 트러블슈팅 가이드 시스템** — 크래시 유형별(ACCESS_VIOLATION, D6DDDA, C++ Exception, 프리징, 로딩 중 크래시, 스냅샷) 단계별 가이드 6개를 자동 매칭.
- Helper: `PreserveFilteredCrashDumps=1` INI 옵션 추가 — 거짓양성 필터가 삭제하려는 덤프를 보존하여 크래시 미감지 원인 진단 가능.
- Helper: Preflight에 비-ESL 플러그인 240개 초과 경고 및 알려진 비호환 모드 조합 체크 추가.
- WinUI: Discord/Reddit 커뮤니티 공유용 이모지+마크다운 포맷 복사 버튼 추가.
- WinUI: 동일 패턴 반복 시 "동일 패턴 N회 반복 발생" 배지 표시.
- WinUI: 접이식 트러블슈팅 체크리스트 UI 추가.
- WinUI: 이벤트 탭에서 `detail` 필드 기반 가독성 높은 포맷으로 표시.

### 수정
- DumpTool: `MissingMasters` 판정의 암묵 런타임 마스터 예외 목록에 `_ResourcePack.esl`/`ResourcePack.esl`를 추가해 false positive 완화.
- DumpTool: 정상 종료 덤프에 대한 CTD/BEES 힌트 억제 — 스냅샷 유사 인시던트에서 크래시 전용 라벨과 권장사항을 게이트.
- DumpTool: CrashLogger 상관 용의자 순위를 우선하도록 랭킹 로직 보정.
- DumpTool: JSON 데이터 파일 로드 시 `version` 필드 필수화 + 잘못된 항목 스킵/경고 로그.
- Helper: 정상 종료(exit_code=0) 시 크래시 뷰어 팝업 억제 강화.
- Helper: exit_code=0이면서 강한 크래시 증거(strong-crash)가 있는 경우 크래시 뷰어를 지연 실행하도록 개선.
- Helper: 크래시 뷰어 실행 결과를 확인하고, 실패 시 Win32 에러코드/경로를 로그에 기록.
- WinUI: bare catch 블록에 진단 로깅 추가.

### 리팩터링
- Helper: `HandleCrashEventTick()` 395줄 → ~130줄로 축소 — 6개 함수 추출 및 종료 예외 판정 로직 통합.
- DumpTool: `internal::` 래퍼 함수 4개를 제거하고 `minidump::` 단일 네임스페이스로 통합.
- DumpTool: EvidenceBuilder 파일 재구성 — `EvidenceBuilderInternals*` → `EvidenceBuilder*`로 간결화.
- DumpTool: AnalysisSummary JSON 파싱 헬퍼 추출, `WideLower` 유틸 통합.

### 테스트
- CrashLogger 파서 엣지케이스 테스트 20개 추가 (기존 18개 → 38개).
- 크래시 캡처 필터/리팩터링 구조 검증 가드 테스트 추가.
- 이벤트 가독성/메뉴 이름 인라인 저장 가드 테스트 10개 추가.
- AnalysisSummary 헬퍼 구조 가드 테스트 추가.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`44/44`).

## v0.2.40 (2026-02-23)

### 수정
- `v0.2.40-rc3`의 누락 마스터(false positive) 완화와 `v0.2.40-rc4`의 Helper/WinUI 안정화 개선을 정식 반영.
- Helper: crash event 재연결 재시도, hang-only 모드 가시화 로그, 프로세스별 singleton mutex 및 plugin watchdog 기반 재기동 경로를 적용.
- WinUI: 분석 취소 경로(out-of-proc headless 포함)와 대용량 아티팩트 비동기 로딩을 적용해 프리징 체감 개선.
- Helper: retention 정리를 백그라운드 워커로 분리해 캡처 핫패스 블로킹을 완화.

### 테스트
- Linux: `cmake --build build-linux-test -j` 성공.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh /home/kdw73/Tullius_ctd_loger /mnt/c/Users/kdw73/Tullius_ctd_loger` 통과.

## v0.2.40-rc4 (2026-02-23)

### 수정
- Helper: crash event 핸들 열기 실패를 상태로 보존하고, 런타임에서 주기적으로 재연결을 시도하도록 보강. crash event 부재 시 hang-only 모드 경고를 로그에 명확히 표기.
- Helper: 프로세스별 singleton mutex를 도입해 중복 helper 실행을 억제하고, plugin/watchdog와의 생명주기 동기화를 강화.
- Plugin: helper auto-start 경로를 재사용 가능한 함수로 정리하고, helper가 내려갔을 때 지수 백오프로 재기동하는 watchdog을 추가.
- Helper: retention 정리를 캡처 핫패스에서 분리해 백그라운드 워커(큐)로 비동기 처리하도록 변경.
- WinUI: 분석 취소 버튼/취소 토큰 경로를 추가하고, 블랙박스·리포트·WCT 로딩을 백그라운드로 이동해 대용량 아티팩트에서 UI 프리징을 완화.
- WinUI: 분석 실행을 out-of-proc headless 경로로 확장해 취소 시 분석 프로세스를 종료할 수 있도록 개선.

### 테스트
- Linux: `cmake --build build-linux-test -j` 성공.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh /home/kdw73/Tullius_ctd_loger /mnt/c/Users/kdw73/Tullius_ctd_loger` 통과.

## v0.2.40-rc3 (2026-02-23)

### 수정
- DumpTool: `MissingMasters` 계산에서 런타임/매니저 상태에 따라 `plugins.txt`에 명시되지 않을 수 있는 기본 마스터(`Skyrim.esm`, `Update.esm`, DLC 3종, 무료 CC 4종)를 암묵 로드 예외로 처리해 false positive를 완화.
- Diagnostics: 프리징 리포트에서 기본 마스터가 대량 누락으로 표시되며 `MISSING_MASTER`가 과도하게 트리거되던 사례를 재현 기준으로 교정.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh` 통과.

## v0.2.40-rc2 (2026-02-23)

### 수정
- Helper(Hang): foreground/not-foreground 억제 판단과 로그 경로를 공통 헬퍼로 정리해 감지/확정 단계 중복 코드를 제거.
- Helper(Process Exit): 종료 처리 분기를 `Drain/Cleanup/Launch` 보조 함수로 분해해 CTD/정상종료 경계 로직의 가독성과 유지보수성을 개선.
- Retention: 출력 디렉터리를 1회 스캔한 결과를 재사용하고 timestamp refcount로 incident manifest 삭제 조건을 계산해 불필요한 재스캔을 제거.
- Tests: 소스 가드 테스트 공통 유틸(`SourceGuardTestUtils.h`)을 도입하고 구조 기반(assert order/body) 검증으로 문자열 취약 가드를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh` 통과.

## v0.2.39 (2026-02-22)

### 수정
- Helper/WinUI: CTD/프리징 이후 DumpTool 뷰어 auto-open 경로를 보강(실패/즉시 종료 시에도 headless 분석이 스킵되지 않도록)하고, 뷰어가 열릴 때 기존 분석 산출물을 우선 로드해 중복 분석을 줄임.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc4 (2026-02-22)

### 수정
- Helper: DumpTool 뷰어 auto-open이 실패하거나 즉시 종료되는 경우에도, headless 분석을 스킵하지 않도록 `viewerNow` 판단을 "실제 런치 성공" 기준으로 보강.
- Helper: 프로세스 종료 후 Hang 뷰어 auto-open 로그가 실제 런치 결과를 반영하도록 수정(실패 케이스에서 오해 방지).
- WinUI: Helper가 headless 분석 산출물(Summary/Report 등)을 생성한 직후 뷰어를 auto-open하는 경우, 뷰어가 재분석을 중복 수행하지 않고 기존 산출물을 먼저 로드하도록 개선(필요 시 "지금 분석"으로 재실행 가능).

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc3 (2026-02-21)

### 수정
- CI: 릴리즈 파이프라인(Linux Unit Tests)에서 `nlohmann/json.hpp`가 없는 환경에서도 빌드가 되도록, `skydiag_plugin_rules_logic_tests`를 조건부로 활성화하도록 수정.
- Helper: 크래시 이벤트 후 프로세스가 `exit_code=0`으로 종료된 경우에도, 강한 예외 코드가 감지되면 덤프/자동 뷰어 오픈을 억제하지 않도록 보강(CTD인데 뷰어가 안 뜨는 체감 완화).
- Helper: DumpTool 뷰어 실행이 즉시 종료되는 케이스를 감지해 `SkyrimDiagHelper.log`에 런타임/시작 크래시 힌트를 남기도록 진단 로그를 보강.
- Release: `-rc` 태그는 GitHub Release를 pre-release로 생성하도록 워크플로우를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc2 (2026-02-21)

### 수정
- Helper: 크래시 이벤트 후 프로세스가 `exit_code=0`으로 종료된 경우에도, 강한 예외 코드가 감지되면 덤프/자동 뷰어 오픈을 억제하지 않도록 보강(CTD인데 뷰어가 안 뜨는 체감 완화).
- Helper: DumpTool 뷰어 실행이 즉시 종료되는 케이스를 감지해 `SkyrimDiagHelper.log`에 런타임/시작 크래시 힌트를 남기도록 진단 로그를 보강.
- Release: `-rc` 태그는 GitHub Release를 pre-release로 생성하도록 워크플로우를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc1 (2026-02-21)

### 개선
- Helper: 시작 시 `SkyrimDiag_Preflight.json`을 생성하는 호환성 프리플라이트 추가. Crash Logger 중복, BEES 필요 조건, 플러그인 스캔 상태를 사전 점검.
- Helper/WCT: COM wait-chain 콜백 등록을 best-effort로 추가해 프리징 분석 맥락을 확장.
- Helper: 덤프 생성 실패 시 `SkyrimDiag_WER_LocalDumps_Hint.txt`를 자동 생성해 WER LocalDumps fallback 가이드를 제공.
- Plugin: 리소스 로깅에 적응형 스로틀 추가(`EnableAdaptiveResourceLogThrottle`)로 대량 loose-file burst 환경에서 오버헤드 완화.
- Plugin/Helper 설정: 신규 옵션(`EnableCompatibilityPreflight`, `EnableWerDumpFallbackHint`, 리소스 스로틀 키) 추가 및 manifest snapshot 반영.
- Release tooling: `scripts/verify_release_gate.sh` 추가로 릴리즈 하드게이트(스크립트 해시/필수 파일/ZIP 엔트리/용량/중첩 경로)를 원샷 검증 가능하게 개선.

### 테스트
- 신규 가드 테스트 추가:
  - `tests/helper_preflight_guard_tests.cpp`
- 기존 가드 테스트 확장:
  - `tests/helper_crash_autopen_config_tests.cpp`
  - `tests/crash_hook_mode_guard_tests.cpp`
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공, 신규 가드 exe 3종 수동 실행 통과.
- Packaging/Release gate:
  - `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` 성공
  - `bash scripts/verify_release_gate.sh` 통과

## v0.2.38-rc3 (2026-02-20)

### 수정
- Helper: pending crash 분석 태스크 정리 시 analyzer 프로세스가 살아 있으면 종료 후 핸들을 닫도록 보강하여, 잔존 프로세스로 인한 재진입/충돌 가능성을 완화.
- Plugin: UI 작업 큐(`AddUITask`) enqueue 실패 예외 가드를 추가해 pending 플래그가 고착되는 런타임 데드락 가능성을 완화.
- Plugin: 리소스 훅에서 관심 확장자(.nif/.hkx/.tri) 선필터를 추가해 불필요한 경로 조합/문자열 처리 오버헤드를 줄임.
- DumpTool: 시그니처 DB 로더를 항목 단위 내결함으로 개선(잘못된 hex/regex/구조 항목 스킵)하고, regex 사전 컴파일을 도입해 매칭 경로 안정성/성능을 보강.
- DumpTool: missing masters 계산 시 비활성 플러그인으로 인한 false positive를 제거.
- Packaging: `dump_tool/data` 하위 파일을 재귀 수집하도록 변경해 신규 데이터 파일이 패키지에서 누락되지 않도록 개선.

### 테스트
- 신규 가드 테스트 추가:
  - `tests/pending_crash_analysis_guard_tests.cpp`
  - `tests/plugin_runtime_guard_tests.cpp`
- 런타임/로직/패키징 회귀 테스트 확장:
  - `tests/analysis_engine_runtime_tests.cpp`
  - `tests/plugin_rules_logic_tests.cpp`
  - `tests/packaging_includes_cli_tests.py`
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(38/38).
- Windows 빌드/패키징 + 릴리즈 하드게이트(WinUI 필수 파일, ZIP 필수 엔트리, 용량/중첩 경로 가드) 통과.

## v0.2.37 (2026-02-17)

### 수정
- Packaging: WinUI 폴더 복사 시 중첩 빌드 산출물(`publish/`, `win-x64/`, `x64/`)이 함께 ZIP에 들어가던 문제 수정. `scripts/package.py`에서 중첩 산출물을 제외하도록 보강해 릴리즈 ZIP 용량 급증(파일 중복 포함) 회귀를 해결.

### 테스트
- `tests/packaging_includes_cli_tests.py`에 중첩 WinUI 산출물(`publish`, `win-x64`) 미포함 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.36 (2026-02-17)

### 수정
- DumpTool: `usvfs_x64.dll` / `uvsfs64.dll`(MO2 VFS 훅 계층)을 훅 프레임워크 목록으로 분류하도록 보강. 해당 모듈이 크래시 원인으로 과도 지목되던 오탐 가능성을 완화.
- DumpTool: 콜스택/스택 스캔 후보 승격 로직에서 MO2 VFS 훅 모듈(`usvfs_x64.dll`, `uvsfs64.dll`)을 CrashLogger/SKSE 런타임과 동일한 특별 처리 대상으로 추가. 비-훅 후보가 있을 때 피해 프레임 소유자를 1순위 원인으로 과도 지목하지 않도록 조정.

### 테스트
- 훅 프레임워크 JSON 테스트에 `usvfs_x64.dll`, `uvsfs64.dll` 항목 검증 추가.
- 훅 프레임워크 가드 테스트에 MO2 VFS 특별 처리(`topIsMo2Vfs`) 회귀 방지 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.35 (2026-02-17)

### 수정
- Packaging/WinUI: `scripts/build-winui.cmd`의 출력 폴더 선택 로직을 보강해, `App.xbf` / `MainWindow.xbf` / `SkyrimDiagDumpToolWinUI.pri`가 포함된 경로만 패키징 대상으로 채택하도록 수정. 일부 환경에서 `x64` 경로가 우선 선택되며 XBF 자산이 빠져 WinUI가 실행 직후 종료되던 회귀를 수정.
- Packaging: `scripts/package.py`에 WinUI 필수 자산 사전 검증을 추가. `App.xbf` / `MainWindow.xbf` / `.pri` 누락 시 ZIP 생성을 실패시켜 깨진 릴리즈 산출물이 배포되지 않도록 가드.

### 테스트
- `tests/packaging_includes_cli_tests.py`를 확장해 WinUI 필수 자산(`App.xbf`, `MainWindow.xbf`, `.pri`)이 ZIP에 포함되는지 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.34 (2026-02-16)

### 수정
- Helper: 크래시 이벤트로 덤프를 생성한 뒤에도 대상 프로세스가 `exit_code=0`으로 정상 종료하면, 해당 크래시 산출물(`.dmp`, `*_SkyrimDiagSummary.json`, `*_SkyrimDiagReport.txt`, incident manifest 등)을 종료 직전에 정리하도록 보강. 이제 "게임은 정상 종료했는데 CTD 리포트/뷰어가 뜨는" 오탐 체감을 줄임.
- Helper: 정상 종료(`exit_code=0`) 경로에서 deferred crash viewer 자동 오픈을 차단하여, 종료 경계 예외로 남은 크래시 덤프 팝업이 뜨지 않도록 조정.
- Helper: 정상 종료 오탐 정리 경로에서 Crash ETW stop을 산출물 삭제보다 먼저 수행하도록 순서를 보정. ETW 파일 생성/manifest 갱신 타이밍 경합으로 `.etl` 잔존 가능성을 완화.
- Helper: 크래시 산출물 정리 시 파일별 삭제 실패(잠금/권한 등)를 에러코드와 함께 Helper 로그에 기록하도록 보강.

### 테스트
- 크래시 오탐 가드 테스트에 정상 종료 후 산출물 정리 로직 문자열 가드 추가 (`tests/crash_capture_false_positive_guard_tests.cpp`).
- 크래시 오탐 가드 테스트에 ETW stop 선행 보장 및 삭제 실패 로그 가드를 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.33 (2026-02-15)

### 수정
- DumpTool: `sl.interposer.dll`(Streamline/DLSS interposer)을 훅 프레임워크 목록으로 분류하도록 보강. 이제 `sl.interposer.dll`을 단독 원인으로 과도 지목하는 오탐을 줄이고, 비-훅 후보/리소스 충돌 단서를 우선 보도록 유도.
- Helper: 크래시 이벤트 직후 3초 내 프로세스가 종료되고, 크래시 시점 상태가 메뉴(`kState_InMenu`)였던 경우를 종료 경계 케이스로 간주하여 자동 액션을 억제. 덤프는 보존하되 자동 뷰어 팝업/자동 headless 분석을 건너뛰어 "게임 종료했는데 크래시 창이 뜨는" 피드백을 완화.
- Helper: incident manifest의 `state_flags`를 크래시 시점 스냅샷으로 고정해 종료 직후 상태 변동으로 인한 맥락 왜곡을 줄임.

### 테스트
- 훅 프레임워크 JSON/가드 테스트에 `sl.interposer.dll` 회귀 방지 검증 추가.
- 크래시 오탐 가드 테스트에 메뉴 경계 억제 플래그(`suppressCrashAutomationForLikelyShutdownException`) 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.32 (2026-02-15)

### 수정
- Hang 분석 오탐 완화: `win32u.dll`을 시스템 DLL 목록에 추가하고, Windows 시스템 경로(`...\Windows\System32\...` 등) 기반 분류를 도입해 스택 후보에서 시스템 DLL이 유력 후보로 과도하게 노출되는 케이스를 줄임.
- 요약 문구 보수화: hang 캡처에서 스택 1순위가 Windows 시스템 DLL일 경우, `유력 원인`으로 단정하지 않고 "대기/피해 위치 가능성, 덤프 단독으로 원인 단정 어려움"으로 안내하도록 조정.
- 권장 조치 보수화: 스택 1순위가 시스템 DLL이면 모드 재설치/비활성화 단정 안내 대신 비-시스템 후보/리소스/충돌 단서 우선 점검을 유도.
- `InferredMod` 안전장치: fault module이 시스템/게임 모듈이거나 추정명이 DLL/EXE 이름 형태일 때는 `inferred_mod_name`을 비워 잘못된 `InferredMod: win32u.dll` 출력 가능성을 차단.
- CrashLogger 파서/후처리의 시스템 DLL 필터에도 `win32u.dll`을 반영해 결과 일관성을 개선.

### 테스트
- 시스템 DLL 오탐 회귀 방지 가드 테스트 추가(`tests/system_module_guard_tests.cpp`).
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.31 (2026-02-15)

### 수정
- DumpTool: `skse64_loader.dll`/`skse64_steam_loader.dll`뿐 아니라 `skse64_1_6_1170.dll` 형태의 SKSE 런타임 DLL(`skse64_*.dll`)도 훅 프레임워크로 판별하도록 보완. 기존에는 런타임 DLL이 일반 원인 후보로 승격되는 오탐이 남아있을 수 있었음.
- DumpTool: 콜스택/스택스캔의 훅 프레임워크 우선순위 완화 로직에서 SKSE 로더 별칭이 아니라 SKSE 런타임 패턴 공통 판별(`IsSkseModule`)을 사용하도록 변경.
- 데이터: `hook_frameworks.json` 기본 목록에 `skse64.dll` 항목 추가.

### 테스트
- 훅 프레임워크 가드 테스트를 SKSE 런타임 공통 판별(`topIsSkseRuntime`, `IsSkseModule`) 기준으로 갱신.
- `hook_frameworks.json` 테스트에 `skse64.dll` 항목 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.30 (2026-02-15)

### 수정
- DumpTool: 스택 후보 승격 로직에서 `skse64_loader.dll` / `skse64_steam_loader.dll`을 CrashLogger 계열과 동일한 훅 프레임워크 특수 케이스로 처리하도록 보완. 비-훅 후보가 있을 때 로더 DLL이 과도하게 1순위로 지목되는 오탐을 완화.
- 요약 문구: 훅 프레임워크 모듈(예: SKSE 로더)만 남는 경우 `유력 원인`으로 단정하지 않고 "피해 위치 가능성 / 단독 원인 단정 어려움"으로 보수화.
- 권장 조치: 훅 프레임워크가 fault module인 상황에서 비-훅 후보가 없으면, 해당 DLL 자체를 단독 원인으로 안내하지 않고 리소스/충돌/비-훅 단서 우선 점검을 유도하도록 조정.

### 테스트
- 훅 프레임워크 가드 테스트에 SKSE 로더 별칭 처리(`topIsSkseLoader`) 검증을 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.29 (2026-02-15)

### 수정
- DumpTool: `CrashLogger.dll`(구/별칭 파일명)도 `crashloggersse.dll`과 동일하게 훅 프레임워크 목록으로 분류하도록 보완. 기존에는 별칭이 목록에 없어 스택 후보 1순위로 과도 지목되는 오탐 케이스가 발생할 수 있었음.
- DumpTool: 스택 기반 후보 승격 로직에서 CrashLogger 특수 처리에 `CrashLogger.dll` 별칭을 추가하여, 비-훅 후보가 있을 때 피해 프레임 소유자를 원인으로 과도 지목하지 않도록 개선.
- 요약/권장 문구: `SkyrimSE.exe` 또는 시스템 모듈 크래시에서 스택 1순위가 훅 프레임워크(`CrashLogger.dll` 포함)인 경우, 단독 원인으로 단정하지 않고 "피해 위치 가능성"을 명시하도록 보수화.
- 권장 조치: 비-훅 스택 후보가 존재하면 해당 후보를 우선 안내하고, 훅 프레임워크 후보만 남을 때는 리소스/충돌/비-훅 단서 우선 점검 가이드를 제공.

### 테스트
- `hook_frameworks.json`에 `crashlogger.dll` 항목 존재를 검증하는 테스트 추가.
- 훅 프레임워크 가드 테스트에 `CrashLogger.dll` 별칭 처리/요약 보수화 가드 케이스 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.28 (2026-02-15)

### 수정
- Retention: `Crash`/`Crash_Full` 덤프가 같은 timestamp를 공유할 때, 한쪽 덤프만 정리되어도 incident manifest(`SkyrimDiag_Incident_Crash_<ts>.json`)가 같이 삭제되던 문제 수정. 동일 timestamp의 다른 덤프가 남아 있으면 manifest를 유지.
- Helper(Hang): ETW 파일 저장 전에 retention이 먼저 실행되어 ETW 개수 제한이 즉시 반영되지 않던 순서 문제 수정. ETW stop/write 이후 retention을 적용하도록 조정.
- Helper(Crash): 비동기 Crash ETW stop 완료 시점과 자동 Full 재캡처 생성 시점에 retention을 즉시 재적용하도록 보완하여, 세션 중에도 덤프/ETW 보관 개수 제한을 더 일관되게 유지.

### 테스트
- `skydiag_retention_tests` 보강:
  - 동일 timestamp sibling crash dump(`Crash` + `Crash_Full`) 시 manifest 유지 회귀 테스트 추가.
  - Crash/Hang ETW trace를 통합 대상으로 개수 제한 prune 동작을 검증하는 테스트 추가.

## v0.2.27 (2026-02-15)

### 수정
- 릴리즈 CI(Linux Unit Tests)에서 `nlohmann/json.hpp`가 없는 환경에서 `skydiag_analysis_engine_runtime_tests` 빌드가 실패하던 문제 수정. 이제 헤더가 있을 때만 해당 런타임 테스트 타깃을 활성화하여 태그 릴리즈 파이프라인이 안정적으로 동작.

### 포함
- 분석 신뢰성 개선(시그니처 DB/주소 해석/크래시 이력/스코어링 교정/오탐 완화) 변경을 그대로 포함.

## v0.2.26 (2026-02-15)

### 추가
- DumpTool: 크래시 시그니처 데이터베이스 도입 (`dump_tool/data/crash_signatures.json`) 및 분석 파이프라인 통합. 예외 코드/모듈/오프셋/콜스택 패턴을 기반으로 알려진 크래시 패턴을 우선 진단.
- DumpTool: 게임 EXE 오프셋 해석기(Address Resolver) 도입 (`dump_tool/data/address_db/skyrimse_functions.json`). 알려진 함수와 매칭되면 증거/요약 JSON에 함수명을 출력.
- DumpTool: 크래시 이력 저장/통계 엔진 도입 (`crash_history.json`). 최근 반복 발생 모듈 통계를 증거와 요약 JSON에 포함.
- 테스트: 핵심 분석 엔진 런타임 테스트 추가 (`tests/analysis_engine_runtime_tests.cpp`) 및 스코어링/시그니처/주소해석/이력 관련 가드 테스트 확장.

### 개선
- 훅 프레임워크 목록을 JSON으로 외부화하고 분석기/패키징 경로를 통합하여 하드코딩 중복 제거.
- 스택 스캔 점수 계산에 RSP 근접 가중치(8/4/2/1)를 적용하고 임계값을 재보정하여 오탐을 완화.
- 결과 JSON(`*_SkyrimDiagSummary.json`)에 `signature_match`, `resolved_functions`, `crash_history_stats`, triage 확장 필드를 추가.

### 수정
- Fallback 모듈 탐지 경로에서 `fault_module_offset`가 누락되던 문제 수정(시그니처 매칭/주소 해석 정확도 개선).
- 시그니처 `callstack_contains` 매칭 입력을 실제 콜스택 프레임 기반으로 보강(기존 suspect 모듈명 중심 입력의 한계 보완).

## v0.2.25 (2026-02-14)

### 수정
- **Helper: 빠르게 종료되는 크래시에서 덤프가 0바이트로 생성되던 문제 수정.** 기존에는 크래시 이벤트 수신 후 최대 4.5초간 필터링(정상 종료/핸들된 예외 확인)을 먼저 수행한 뒤 덤프를 시도했으나, 그 사이 프로세스가 종료되면 `MiniDumpWriteDump`가 실패하여 빈 파일만 남았음. 이제 **덤프를 즉시 먼저 쓰고**, 사후에 false positive를 필터링(정상 종료 시 덤프 삭제)하는 "dump-first" 전략으로 변경.
- Helper: 덤프 실패 시 0바이트 파일을 자동 삭제하고, 실패 원인을 Helper 로그 파일에 기록하도록 개선 (기존에는 stderr에만 출력).
- DumpTool: CrashLoggerSSE/기타 훅 프레임워크가 유력 후보 1순위로 과도 노출되던 케이스 완화. 스택 후보 정렬에서 훅 프레임워크(특히 `CrashLoggerSSE.dll`)를 보수적으로 비우선화하고, 훅 프레임워크 1순위일 때 CrashLogger 근거 기반 confidence 부스트를 억제하여 오탐 안내를 줄임.
- Helper: crash event가 수동 리셋(manual-reset)인데 소비(reset)하지 않아 동일 신호를 반복 처리하던 루프를 수정. 이벤트 핸들을 `EVENT_MODIFY_STATE|SYNCHRONIZE`로 열고 처리 직후 `ResetEvent`로 소비하여 중복 처리/지연 루프를 방지.
- Helper: handled first-chance 예외 필터를 보수화. heartbeat 1회 전진만으로 덤프 삭제하지 않고, 다중 체크에서 2회 이상 전진이 확인될 때만 삭제하여 실제 크래시 누락 위험을 낮춤.

## v0.2.23 (2026-02-14)

### 수정
- Helper: `SkyrimDiagHelper.log`가 게임 세션 간에 계속 누적되던 문제 수정. 새 게임 세션(프로세스 어태치) 시 로그 파일을 초기화하여 매번 깨끗한 로그로 시작.

### 내부 개선
- Helper: 미사용 파라미터 `attachHeartbeatQpc` 제거 (내부 API 정리).
- Helper: 하트비트 초기화 경고 지연시간을 명명된 상수 `kHeartbeatInitWarnDelaySec`로 추출.

## v0.2.22 (2026-02-14)

### 수정
- Helper: 하트비트가 어태치 이후 전진하지 않으면 자동 행(hang) 캡처가 영구 비활성화되던 문제 수정. 기존의 `heartbeatEverAdvanced` 가드를 제거하고, 플러그인 하트비트 초기화 여부(`last_heartbeat_qpc != 0`)만 확인하도록 변경. 프리즈 시 하트비트가 멈추는 것이 정상 신호이므로, 데드락/무한루프/무한로딩 시나리오에서 자동 캡처가 올바르게 작동.
- Helper: 게임이 프리즈된 상태에서 Alt-Tab하면 포그라운드 억제(`SuppressHangWhenNotForeground`)로 행 덤프가 생성되지 않던 캐치-22 수정. 포그라운드가 아닐 때 윈도우 응답성(`IsWindowResponsive`)을 함께 확인하여, 윈도우가 무응답이면(진짜 프리즈) 억제하지 않고 캡처 진행.
- WinUI: 내부 리스트(증거/콜스택/이벤트 등)와 외부 페이지 스크롤이 동시에 굴러가던 문제 수정. 내부 리스트가 스크롤 경계(상단/하단)에 도달했을 때만 외부 스크롤로 전환.

## v0.2.21 (2026-02-14)

### 수정
- Helper: 핸들링된 첫 번째 기회 예외(first-chance exception)로 인한 오탐 덤프 생성 방지. 크래시 이벤트 수신 후 프로세스가 살아있을 때 하트비트 갱신 여부를 확인하여, 게임이 정상 동작 중이면 덤프를 건너뛰도록 개선.

## v0.2.20 (2026-02-13)

### 추가
- DumpTool: 알려진 훅 프레임워크 모드(EngineFixes, SSE Display Tweaks, po3_Tweaks, HDT-SMP, CrashLoggerSSE 등)가 fault module일 때 confidence를 한 단계 낮추고, "다른 모드의 메모리 오염 피해자일 수 있음" 경고를 Summary와 Recommendations에 표시. 훅 모드가 단순히 크래시 발생 위치일 뿐 진짜 원인이 아닐 수 있음을 사용자에게 안내.

## v0.2.19 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프 생성 억제 강화. 종료 대기 시간을 500ms→3000ms로 증가하여, 모드가 많은 환경에서 DLL 정리 시간이 길어도 정상 종료로 올바르게 판단.
- Helper: 크래시 후 프로세스가 늦게 종료되는 경우 뷰어가 열리지 않던 문제 수정. 프로세스 종료 시점까지 뷰어 실행을 지연(deferred)하여, C++ 예외 등으로 프로세스가 지연 종료되어도 뷰어가 자동으로 열리도록 개선.

## v0.2.18 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프가 생성되던 문제 수정. 종료 과정에서 DLL 정리 중 발생하는 예외를 VEH가 감지하여 덤프를 만들던 현상을, 프로세스 종료 코드(exit_code=0)를 확인해 정상 종료로 판단하면 덤프를 건너뛰도록 개선.

## v0.2.17 (2026-02-13)

### Fixed
- Build: correct MSVC runtime library generator expression in CMake.
- Build: add `/utf-8` compiler flag for MSVC to satisfy fmt v11 requirement.
- Build: handle x64 platform subfolder in WinUI output path.
- Build: explicit exit code 0 after robocopy in `build-winui.cmd`.
- CI: build all test targets instead of hardcoded list.
- CI: add tag-triggered release workflow.
- Tests: remove assertions for unimplemented features.

## v0.2.16 (2026-02-13)

### Fixed
- Helper: fix race condition where crash event was missed if the game process terminated before the next poll cycle. On process exit, the helper now drains any pending crash event (non-blocking) before shutting down.

## v0.2.15 (2026-02-10)

### Fixed
- WinUI DumpTool: surface native analysis exceptions with actionable messages instead of a generic "External component has thrown an exception."
  - When a managed exception occurs during native interop, a `*_SkyrimDiagNativeException.log` is written to the output folder (best-effort).
- DumpTool: fix a rare analysis failure when merging existing summary triage (`[json.exception.invalid_iterator.214] cannot get value`).
- DumpTool: manual snapshot captures (`SkyrimDiag_Manual_*.dmp`) are now more reliably classified as snapshots (not CTDs) unless an exception stream is present.
- DumpTool: do not generate a misleading crash bucket key for snapshot dumps that have no exception/module/callstack information.

## v0.2.14 (2026-02-10)

### Changed
- CrashLogger integration: if CrashLogger.ini sets `Crashlog Directory`, SkyrimDiag will also search that folder when auto-detecting CrashLogger logs (best-effort).

### Added
- Internal regression tests: parse CrashLogger.ini `Crashlog Directory` (quotes/spacing/comments).

## v0.2.13 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool evidence builder internals into smaller modules (no behavior changes).

### Added
- Internal regression tests: harden CrashLogger parser fixtures for v1.20 format variations (callstack rows + version header variants).

## v0.2.12 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool analyzer internals into smaller modules (no behavior changes).
- Internal refactor only: split Helper main into smaller modules (no behavior changes).

## v0.2.11 (2026-02-10)

### Changed
- Avoid duplicate analysis: when Helper auto-opens the WinUI viewer for a dump, it now skips headless auto-analysis for that same dump.

### Added
- New regression test: `tests/headless_analysis_policy_tests.cpp`

## v0.2.10 (2026-02-10)

### Added
- Headless analyzer CLI: `SkyrimDiagDumpToolCli.exe` (no WinUI dependency) for post-incident analysis.
- Helper now prefers the headless CLI for auto-analysis when available, and falls back to the WinUI exe for backward compatibility.
- Packaging now ships `SkyrimDiagDumpToolCli.exe` next to `SkyrimDiagHelper.exe`.
- New tests:
  - `tests/dump_tool_cli_args_tests.cpp`
  - `tests/dump_tool_headless_resolver_tests.cpp`
  - `tests/packaging_includes_cli_tests.py`

## v0.2.9 (2026-02-10)

### Added
- Incident manifest sidecar JSON per capture (enabled by default):
  - `SkyrimDiag_Incident_Crash_*.json`
  - `SkyrimDiag_Incident_Hang_*.json`
  - `SkyrimDiag_Incident_Manual_*.json`
  - Includes `incident_id`, `capture_kind`, artifact filenames, ETW status, and an optional privacy-safe config snapshot.
- Optional crash-window ETW capture in `SkyrimDiagHelper.ini` (advanced, OFF by default):
  - `EnableEtwCaptureOnCrash`
  - `EtwCrashProfile`
  - `EtwCrashCaptureSeconds` (1..30)
- DumpTool now surfaces incident context in summary/report when a manifest is present (`summary.incident.*`).

### Changed
- Retention cleanup now prunes incident manifests alongside their corresponding dumps, and will remove `SkyrimDiag_Crash_*.etl` traces when pruning crash dumps.

## v0.2.8 (2026-02-10)

### Added
- Crash hook safety guard option in `dist/SkyrimDiag.ini`:
  - `EnableUnsafeCrashHookMode2=1` is now required to use `CrashHookMode=2`.
- Online symbol source control in `dist/SkyrimDiagHelper.ini`:
  - `AllowOnlineSymbols=0|1` with default `0` (offline/local cache).
- DumpTool privacy telemetry fields in summary/report outputs:
  - `path_redaction_applied`
  - `online_symbol_source_allowed`
  - `online_symbol_source_used`
- New regression tests:
  - `tests/crash_hook_mode_guard_tests.cpp`
  - `tests/symbol_privacy_controls_tests.cpp`
- Added vibe-kit guard workflow and doctor script scaffolding:
  - `.github/workflows/vibekit-guard.yml`
  - `.vibe/brain/agents_doctor.py`

### Changed
- DumpTool symbolization now defaults to offline/local cache unless explicitly opted in.
- Helper now passes explicit symbol policy flags (`--allow-online-symbols` / `--no-online-symbols`) to WinUI analyzer path.
- Path redaction is applied more consistently in outputs, including resource path lines.
- Test runner wiring now uses `Python3_EXECUTABLE` and `sys.executable` for cross-platform Python invocation.
- Vibe-kit seed/config scripts and docs were refreshed:
  - `.vibe/config.json`
  - `.vibe/README.md`
  - `.vibe/brain/*`
  - `scripts/setup_vibe_env.py`
  - `scripts/vibe.py`

### Fixed
- Windows `ctest` compatibility issue caused by hardcoded `python3` in bucket quality script tests.

## v0.2.6 (2026-02-07)

### Added
- Helper retention/disk cleanup options in `SkyrimDiagHelper.ini`:
  - `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
  - `MaxHelperLogBytes`, `MaxHelperLogFiles`
- Crash viewer popup suppression options in `SkyrimDiagHelper.ini`:
  - `AutoOpenCrashOnlyIfProcessExited`, `AutoOpenCrashWaitForExitMs`
- DumpTool evidence: exception parameter analysis for common codes (e.g., access violation read/write/execute + address).
- CrashLogger integration: detect and report CrashLogger version string (e.g., `CrashLoggerSSE v1.19.0`) when a log is auto-detected.
- WinUI: added a "Copy summary" action for quick sharing.

### Fixed
- CI Linux workflow now builds all unit test targets before running `ctest`.
- CI Windows manual workflow builds the WinUI shell before packaging.

## v0.2.5 (2026-02-06)

### Fixed
- Packaging bug in `scripts/package.py`: WinUI publish output is now copied recursively, preventing runtime file loss when publish layouts include nested files/directories.
- WinUI packaging crash fix: `scripts/build-winui.cmd` now stages from WinUI build output (includes required `.pri/.xbf` assets) instead of stripped publish output.
- WinUI visual quality improvements: enabled Per-Monitor V2 DPI awareness via app manifest for sharper rendering on high-DPI displays.
- WinUI scrolling reliability: when nested controls consume mouse wheel input, wheel events are chained to the root scroll viewer for smoother page scrolling.
- WinUI localization polish: static UI labels/buttons now switch between English/Korean (`--lang ko` or system UI language Korean).

### Added
- Native analyzer bridge DLL for WinUI (`SkyrimDiagDumpToolNative.dll`) with exported C ABI (`SkyrimDiagAnalyzeDumpW`) so WinUI can analyze dumps directly without launching legacy UI executable.
- Built-in advanced analysis panels in WinUI (callstack, evidence, resources, blackbox events, WCT JSON, report text) in the same window as beginner summary.

### Changed
- WinUI headless mode now runs native analysis directly (no process delegation to `SkyrimDiagDumpTool.exe`).
- Helper dump-tool resolution no longer falls back to legacy executable.
- CMake build no longer defines the legacy `SkyrimDiagDumpTool` Win32 executable target (native DLL + WinUI only).
- WinUI publish switched to framework-dependent/lightweight output (`scripts/build-winui.cmd`), reducing package size but requiring user runtimes.
- WinUI viewer visuals refreshed (typography, spacing, card styling, and list readability) while preserving existing dump-analysis workflow.
- WinUI viewer theme refreshed with a Skyrim-inspired parchment + dark stone look.
- WinUI viewer redesigned again using current Fluent/observability UI patterns:
  - fixed left navigation pane visibility (always expanded labels, no icon-only collapse)
  - added quick triage strip (primary suspect/confidence/actions/events)
  - added explicit 3-step workflow cards in Analyze panel
  - increased visual depth with layered surface tokens (`Window/Pane/Hero/Section/Elevated`)
- Packaging now ships full-replacement WinUI set:
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
  - no longer requires or packages `SkyrimDiagDumpTool.exe` / `SkyrimDiagDumpTool.ini`

## v0.2.4 (2026-02-06)

### Added
- New modern WinUI 3 viewer shell (`SkyrimDiagDumpToolWinUI.exe`) with beginner-first layout:
  - dump picker + one-click analysis
  - crash snapshot card (summary/bucket/module/mod hint)
  - top cause candidates list
  - recommended next-step checklist
  - quick action to open legacy advanced viewer
- Windows helper script `scripts/build-winui.cmd` to publish WinUI viewer in self-contained mode.
- Packaging enhancement: `scripts/package.py` now auto-includes WinUI publish artifacts when found (configurable with `--winui-dir` and `--no-winui`).

### Changed
- Helper default dump viewer executable changed to `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`.
- Helper executable resolution now safely falls back to legacy `SkyrimDiagDumpTool.exe` if WinUI executable is missing.
- README and default ini guidance updated for WinUI-first workflow.

## v0.2.3 (2026-02-06)

### Added
- Crash bucketing key (`crash_bucket_key`) output in Summary JSON/Report, plus callstack symbolization improvements to better group repeated CTDs by signature.
- Beginner-first DumpTool UX:
  - Default beginner view with primary CTA (`Check Cause Candidates` / `원인 후보 확인하기`)
  - Top-5 candidate + evidence presentation
  - Explicit `Advanced analysis` toggle to access full tabs.
- DumpTool single-window reuse path: when already open, new dump opens in the same window via inter-process message handoff (`WM_COPYDATA`) instead of creating extra windows.
- New helper viewer auto-open policy options in `SkyrimDiagHelper.ini`:
  - `AutoOpenViewerOnCrash`
  - `AutoOpenViewerOnHang`
  - `AutoOpenViewerOnManualCapture`
  - `AutoOpenHangAfterProcessExit`
  - `AutoOpenHangDelayMs`
  - `AutoOpenViewerBeginnerMode`
- Optional ETW capture around hang dumps (`EnableEtwCaptureOnHang`, `EtwWprExe`, `EtwProfile`, `EtwMaxDurationSec`) as best-effort diagnostics.
- New bucket unit test target (`skydiag_bucket_tests`) and test source (`tests/bucket_tests.cpp`).

### Changed
- Helper dump flow now separates headless analysis from viewer launch:
  - Crash: viewer can open immediately
  - Hang: latest hang dump can be queued and auto-opened after process exit (with configurable delay)
  - Manual capture: viewer auto-open remains off by default.
- DumpTool now persists beginner/advanced default mode in `SkyrimDiagDumpTool.ini` (`BeginnerMode=1|0`) and supports CLI overrides (`--simple-ui`, `--advanced-ui`).

## v0.2.2 (2026-02-03)

### Fixed
- Further reduced Alt-Tab false hang dumps: after returning to foreground, keep suppressing hang dumps while the game window is responsive (and not in a loading screen), until the heartbeat advances.

### Added
- CrashLogger SSE/AE v1.18.0 support: parse and surface the new `C++ EXCEPTION:` details (Type / Info / Throw Location / Module) in evidence, reports, and JSON output.
- DumpTool i18n (EN/KO): English-first UI/output for Nexus + in-app language toggle (persists via `SkyrimDiagDumpTool.ini` and supports CLI `--lang en|ko`).
- DumpTool UI polish: modern owner-draw buttons, better padding (Summary/WCT), WCT mono font, evidence row striping, and Windows 11 rounded corners (best-effort).

## v0.2.1 (2026-02-01)

### Fixed
- Further reduced false hang dumps around Alt-Tab / background pause by keeping suppression “sticky” until the heartbeat advances, and adding a short foreground grace window (`ForegroundGraceSec`).
- Improved CrashLogger SSE/AE log compatibility (v1.17.0+): better detection and parsing for thread dump logs (`threaddump-*.log`) and stack-trace edge cases.

### Added
- Lightweight cross-platform unit tests for hang suppression logic and CrashLogger log parsing core (Linux-friendly, no Win32 deps).

## v0.2.0 (2026-02-01)

### Fixed
- Prevented false hang dumps when the user Alt-Tabs: by default, hang capture is suppressed while Skyrim is not the foreground window (`SuppressHangWhenNotForeground=1`).
- Reduced false positives around menus/shutdown by using a more conservative menu threshold (`HangThresholdInMenuSec`) and a short re-check grace period before writing hang dumps.

### Changed
- DumpTool internal architecture: split into `SkyrimDiagDumpToolCore` (analysis/output) + `SkyrimDiagDumpTool` (UI) to reduce coupling and make future maintenance safer.
- Improved documentation for beta testing and common misinterpretations (manual snapshot vs. real CTD/hang).

## v0.1.0-beta.1 (2026-01-30)

- Initial public beta release.
