# ADR-0021: Engine-Lock Deadlocks Between Game Threads

## Status
Accepted

## Context
게임 엔진은 자기 잠금(`BSSpinLock`, `BSReadWriteLock`)을 기다릴 때 OS 대기 객체를 쓰지 않고, 잠금을 다시 시도하기 전에 `Sleep`으로 잠깐 잡니다. 그래서 게임 스레드끼리 엔진 잠금에서 교착 상태에 빠지면 WCT에는 순환 대기가 보이지 않고, 각 스레드는 `ntdll!NtDelayExecution ← KERNELBASE!SleepEx ← SkyrimSE.exe` 모양으로 잠들어 있을 뿐입니다. 메인 스레드는 그 작업을 기다리는 `engine_wait`로 보이고, 결론은 스택에 우연히 있던 모드를 가리키거나 "모름"이 됩니다.

1.5.97에는 공개된 분석과 수정 모드가 있는 교착 상태가 셋 있습니다(원문은 아래 참고, 사실만 우리 말로 옮김).

1. 셀 로딩 잠금 순서 교착(Skyrim Cell load Freeze fix NG, Nexus 160704). 잡 스레드 하나는 `FUN_1406d3ff0+0x39C`에서 읽기/쓰기 잠금(RVA `0x2F3B8F0`)을 읽기로 잡은 채 스핀 잠금(`0x2F3B8E8`)을 기다리고(`BSSpinLock` 대기 `+0x132C5A` ← `+0x6D9750` ← `+0x6D4A48` ← `+0x6D43EC`), 셀 로더 스레드는 그 스핀 잠금을 쥔 채 읽기/쓰기 잠금을 쓰기로 잡으려 기다립니다(`LockForWrite` `+0xC073AB` ← `+0x6D34BB` ← `+0x60944A` ← `+0x5CDD80` ← `+0x6929DB`).
2. 셀 로딩 읽기→쓰기 자기 교착(같은 모드 0.0.4에서 수정). 한 스레드가 `+0x39C`에서 `0x2F3B8F0`을 읽기로 잡고, 같은 잠금을 쓰기로 다시 잡으려 기다립니다(`+0xC073AB` ← `+0x6D356B` ← `+0x5F0E5E` ← `+0x27DAB3` ← … ← `+0x6D4A31` ← `+0x6D43EC`).
3. 작업 스레드 스핀 잠금 ABBA 교착(WorkerSpinLockFix, Nexus 180884). 작업 스레드 하나는 LockA(`0x2EFF8E0`)를 쥐고 LockB(`0x2F3B8E8`)를, 다른 하나는 LockB를 쥐고 LockA를 기다립니다. 둘 다 `+0x132C5A`에서 잡니다. 메인 스레드는 작업 완료 이벤트를 기다리다 함께 멈춥니다.

RVA는 원문의 `name_1401XXXXX+off`(반환 주소)에서 `주소 − 0x140000000`으로 다시 계산해 확인했습니다. AE용 수정은 Address Library ID로 같은 함수를 찾지만, 우리가 기준으로 삼을 함수 안의 반환 오프셋은 AE에 대해 공개되지 않았으므로 알려진 원인 시그니처는 1.5.97만 둡니다.

잠금 소유자를 읽으려면 엔진의 쓰기 가능한 데이터가 필요한데, 2026-10-22 1.6.1170 실사고 프리징 덤프에는 SkyrimSE.exe 이미지 중 코드(RVA `0x1000..0x174E938`)만 들어 있었습니다.

## Decision

1. **헬퍼**(`helper/src/HangObjectMemory.cpp`, 공통 규칙은 `shared/SkyrimDiagGameLocks.h`): ADR-0010 부록의 프리징·수동 덤프 수집기(PSS 스냅샷과 전체 메모리 덤프 제외)에 붙입니다.
   - 대상: 시작 주소가 게임 EXE 안인 스레드(메인 스레드 포함) 중, 덤프 작성기가 넘긴 컨텍스트의 RIP가 `ntdll!NtDelayExecution` 스텁 시작부터 0x20바이트 안인 스레드. 스텁 주소는 헬퍼 자신의 ntdll에서 얻고(ntdll은 한 부팅 안에서 모든 프로세스에 같은 주소), 게임 쪽 스텁 첫 16바이트가 같을 때만 씁니다.
   - 값: 비휘발 레지스터 8개(RBX, RBP, RSI, RDI, R12–R15)와 RSP부터 0x400바이트의 스택 값.
   - 범위: 게임 EXE 헤더의 섹션 표에서 쓰기 가능하고 실행 불가인 섹션 안을 가리키는 4바이트 정렬 값마다 64바이트 정렬 창(8바이트가 창 끝을 넘으면 다음 창도). 같은 창은 한 번만. 상한: 스레드당 창 32개, 전체 128개·256KB, 시간 20ms. 창을 하나라도 담으면 분석기가 섹션을 알도록 EXE 헤더 첫 0x1000바이트도 담습니다.
   - 덤프 작성기가 스레드를 정지시켜 둔 첫 `MemoryCallback`에서 객체 메모리보다 먼저 모읍니다. 모든 읽기와 창은 `VirtualQueryEx`로 커밋·읽기 가능·guard 아님(창은 이미지 페이지)을 확인한 것만. 잠금 바이트는 4MB 전체 상한에 함께 셉니다. 실패하면 범위가 없을 뿐 덤프는 그대로입니다.
   - 헬퍼 로그에 캡처마다 `Lock memory: threads=<n> pointers=<n> ranges=<n> bytes=<n> time_us=<n>`(상한에 걸리면 `(budget reached)`). 스텁을 확인하지 못하면 줄이 없습니다.
2. **분석기**(`dump_tool/src/GameLockWait.cpp`, `Analyzer.CaptureInputs.cpp`): 프리징에서 메인 스레드와 게임이 시작한 스레드(메인 스레드를 모르는 덤프에서도 게임 스레드는 봄)의 스택 위쪽이 Sleep 계열이고 그것을 부른 첫 비시스템 프레임이 게임 EXE이면(`ClassifyMainThreadWait`의 `engine_wait`/`sleep`) 그 스레드를 엔진 잠금 대기로 봅니다.
   - 스택 값은 잠금 함수 자신의 프레임까지만 봅니다: RSP부터, 그 함수를 부른 호출자로 돌아갈 반환 주소가 든 칸 앞까지. 그보다 위는 호출자들의 프레임이라 이미 놓은 잠금을 가리키는 옛 값이 남아 있을 수 있기 때문입니다. 그 칸을 0x400바이트 안에서 찾지 못하면 레지스터만 봅니다(헬퍼는 그대로 0x400바이트를 담음).
   - 같은 레지스터·스택 값 중 덤프 헤더의 쓰기 가능 섹션 안을 가리키고, 덤프 자체 메모리(로컬 파일 아님)에서 읽은 8바이트가 다른 스레드가 쥔 잠금이면 후보입니다: `BSSpinLock {owner, count}`에서 owner가 덤프의 스레드이고 자신이 아니며 1 ≤ count ≤ 0xFFFF, 또는 `BSReadWriteLock {writer, lock}`에서 쓰기 플래그(0x80000000)와 1 이상의 깊이. 레이아웃은 CommonLibSSE-NG `RE/B/BSAtomic.h`. 읽기로만 잡힌 읽기/쓰기 잠금은 소유 스레드를 담지 않고 `{0, n}`은 다른 데이터와 구별되지 않으므로 일반 후보가 아닙니다.
   - 후보 소유자가 역시 엔진 잠금에서 잠든 스레드이면 대기 간선입니다. 이 간선들이 순환하면(스레드 순서대로 처음 찾은 순환) 엔진 잠금 교착입니다. 대기자 하나, 소유자가 실행 중인 경우, 스택에 남은 옛 포인터가 가리키는 풀린 잠금, 자기가 쥔 잠금은 순환을 만들지 않습니다.
   - 순환이 없으면, 1.5.97의 알려진 패턴(아래 3)이 스택과 잠금 메모리로 일치할 때만 교착으로 봅니다. 읽기 잠금을 쥔 스레드는 메모리로 알 수 없지만, 그 스레드의 반환 주소가 잠금을 잡은 지점 뒤에 있다는 것이 알려진 분석의 근거와 같기 때문입니다. 상태 하나만 보고 교착을 단정하는 시그니처는 이 경로에 쓰지 않습니다(스레드를 지정한 시그니처만).
3. **새 프리징 상태 `engine_lock_deadlock`**(근거 항목만이 아니라 상태로 둠): 엔진 안의 교착이 프리징을 다 설명하고, 상태로 두지 않으면 요약·빠른 답·다음 조치가 스택의 모드를 가리키기 때문입니다.
   - 우선순위: modal 대화상자, Crash Logger 스레드 덤프 교착 다음, `engine_spin_nan`과 WCT `deadlock_likely`보다 앞. 신뢰도 중간.
   - 어떤 모드도 원인으로 지목하지 않습니다: 스택 후보는 낮음과 "호출 경로" 이유로 내리고, 실행 우선 후보는 이력 신호만, 관련 후보·최근 모듈 없음, 메인 스레드가 기다린 게임 스레드 기록(`blocked_*`)은 지우고, 스레드 그룹 합의를 만들지 않으며, 이력 후보 키도 기록하지 않습니다.
   - 요약 문장, 프리징 이유, 빠른 답(`case_id = engine_lock_deadlock`), `[Main thread]`/`[메인 스레드]` 다음 조치(모드를 빼지 말 것, 반복되면 엔진 수정 모드를 찾을 것), 근거 항목(순환과 각 스레드의 대기 위치)에 씁니다.
   - 버킷: `HANG1-` + `v=1|kind=engine_lock_deadlock|locks=<exe>+0x<rva>,…`(정렬, 중복 제거), 알려진 패턴이면 `|pattern=<id>`.
4. **시그니처**(`crash_signatures.json`, `SignatureDatabase.cpp`): 프리징 시그니처에 매칭 키 세 개를 더합니다.
   - `game_lock_threads`: 1–4개, 각각 `{"frames": ["SkyrimSE.exe+0x132C5A", …]}`. 첫 프레임은 Sleep을 부른 프레임과 같아야 하고, 나머지는 그 아래에서 순서대로 나와야 합니다(사이에 다른 프레임 허용). 서로 다른 스레드에 배정합니다. `freeze_state`와 `game_version`이 있어야 하고, 항목 안의 다른 키는 항목을 버립니다.
   - `game_locks`: `{"rva", "kind": "spin"|"rw", "owner_thread": <스레드 번호>}` 또는 `{"rva", "kind": "rw", "state": "read"}`. spin은 소유 스레드, rw는 쓰기 소유 스레드 또는 읽기 잠금(writer 0, 쓰기 플래그 없음, 1 이상의 reader)이 정확히 하나. 덤프에 그 메모리가 없으면 일치하지 않습니다.
   - `game_lock_cycle: true`: 배정된 스레드가 모두 일반 순환 안에 있어야 합니다.
   - 새 항목(모두 `root_cause`, 1.5.97만, 상태 `engine_lock_deadlock`):
     - `CELL_LOAD_LOCK_ORDER_DEADLOCK_1_5_97`(중간): 스레드 0 `+0x132C5A, +0x6D9750, +0x6D4A48, +0x6D43EC`, 스레드 1 `+0xC073AB, +0x6D34BB, +0x60944A, +0x5CDD80, +0x6929DB`, `0x2F3B8E8` 스핀 잠금 소유 = 스레드 1, `0x2F3B8F0` 읽기로 잡힘. 수정 DLL `skyrim-freeze-fix.dll`(원 저장소 xmake 대상 이름, Nexus 배포본 v0.0.5를 설치한 MO2 폴더에서 같은 이름 확인).
     - `CELL_LOAD_READ_WRITE_SELF_DEADLOCK_1_5_97`(중간): 스레드 0 `+0xC073AB, +0x6D356B, +0x5F0E5E, +0x27DAB3, +0x6D4A31, +0x6D43EC`, `0x2F3B8F0` 읽기로 잡힘. 0.0.4 이상을 권함.
     - `WORKER_SPINLOCK_ABBA_DEADLOCK_1_5_97`(높음): 순환 필요, 스레드 0 `+0x132C5A, +0x6D9750, +0x297DDB, +0x2971A4`(LockA 소유), 스레드 1 `+0x132C5A, +0x296C3D, +0x6025FB, +0x6EF480`(LockB 소유). 수정 DLL `WorkerSpinLockFix.dll`(설정 파일 주석과 CMake 프로젝트 이름), 권장 설정 `[phase4_defer] enabled = true`(기본값).
   - 권장 조치는 `[알려진 원인]`/`[Known cause]`와 Nexus 링크, 수정 DLL이 이미 로드됐으면 업데이트·설정 확인·제작자 보고.

## Output Contract

- 요약 JSON `freeze_analysis.game_lock_waits[]`(후보가 있는 스레드와 교착 스레드만, 없으면 키 없음): `tid`, `is_main_thread`, `wait_site`(`SkyrimSE.exe+0x132c5a`), `wait_module`, `wait_offset`, `frames[]`(대기 위치부터 최대 16), `candidate_locks[]`(`location` `RBX`/`RSP+88`, `rva` `0x2f3b8e8`, `kind` `spin`/`rw_write`, `owner_tid`, `count`, `owner_waiting`).
- 요약 JSON `freeze_analysis.engine_lock_deadlock`(교착일 때만): `kind`(`cycle`/`known_pattern`), `threads[]`, `cycle[]`(`tid`, `lock`, `lock_rva`, `kind`, `owner_tid`), `known_pattern`.
- `freeze_analysis.state_id = engine_lock_deadlock`, `quick_answer.case = engine_lock_deadlock`(알려진 패턴이면 `known_issue`).
- 리포트 FreezeAnalysis 아래 `  game_lock_wait tid=… main_thread=… site=… lock=<위치>:<rva>:<kind>:owner=<tid>:count=<n>[:owner_waiting]`, `  engine_lock_deadlock threads=… cycle=… pattern=…`.
- 헬퍼 로그 `Lock memory: …` 한 줄.

## Consequences

### Positive
- 엔진 잠금 교착이 "엔진 안의 교착, 모드 지목 없음"으로 나오고, 1.5.97의 알려진 세 경우는 수정 모드를 바로 권합니다.
- 버전에 묶이지 않는 순환 판정은 어느 런타임에서도 동작합니다.

### Limitations
- 이 버전 이전 덤프에는 잠금 메모리와 헤더가 없어 아무것도 나오지 않습니다(보관 덤프 19건 재분석, 아래).
- 실제 교착 프리징에서 판정이 맞게 나오는지는 확인하지 못했습니다(합성 테스트만). 1.5.97의 알려진 패턴은 1.5.97 덤프로 맞춰 본 적이 없고, 바이너리 대조(아래 검증)만 했습니다. 잠금 창 수집은 실제 게임에서 동작했지만, 게임이 정상일 때의 수동 캡처라 교착은 없었습니다.
- 잠금 대기 중 Sleep 사이 사용자 모드 코드에 있던 순간 덤프가 찍히면 그 스레드는 대기로 보이지 않습니다(1ms 이상 자는 동안이 대부분이므로 드묾).
- 후보는 레지스터와 잠금 함수 프레임의 스택 값에서 추정합니다. 두 스레드가 그 범위 안에 서로 상대가 쥔 잠금을 가리키는 옛 값을 우연히 갖고, 둘 다 다른 이유로 잠들어 있으면 잘못된 순환이 나올 수 있습니다(신뢰도 중간).
- `BSSpinLock::Lock`은 개수를 먼저 쓰고(CAS) 소유 스레드를 나중에 씁니다(CommonLibSSE-NG `BSAtomic.cpp`). 그 사이에 캡처되면 잠금이 직전 소유 스레드의 것으로 읽힙니다. 교착은 오래 유지되는 상태라 이 순간에 걸릴 일은 드물고, 순환이 되려면 두 간선이 모두 맞아야 하므로 따로 거르지 않습니다.
- 읽기로 잡힌 잠금이 끼는 교착은 알려진 패턴으로만 나옵니다.
- AE의 알려진 패턴은 함수 안 반환 오프셋이 공개되지 않아 넣지 않았습니다. 일반 순환 판정은 AE에서도 동작합니다.

## Verification
- `skydiag_game_lock_wait_tests`(Linux): PE 섹션 표, 창 선택(정렬, 경계, 중복, 스레드·전체·바이트 상한), 잠금 해독, FreezeLogger 사례 모양의 ABBA 순환(옛 값·실행 중 소유자 섞임), 순환 아님(대기자 하나, 사슬, 풀린 잠금, 자기 잠금, 덤프에 없는 소유자, 읽기 잠금, 데이터 없음), Sleep이 아닌 대기 제외, 세 스레드 순환과 순서.
- `skydiag_analysis_engine_runtime_tests`: 번들 DB의 세 항목 일치와 근접 불일치(다른 버전, 빠진 앵커, 다른 대기 위치, 소유자 틀림, 풀린 잠금, 쓰기로 잡힘, 메모리 없음, 한 스레드에 두 앵커, 다른 상태, 순환 없음, 소유 뒤바뀜), 잘못된 항목 거부.
- `skydiag_freeze_candidate_consensus_tests`, `skydiag_quick_answer_tests`, `skydiag_bucket_tests`: 상태·이유·후보 없음, 빠른 답·요약·다음 조치·근거·요약 JSON·리포트, 알려진 패턴의 `[Known cause]`, 버킷 키.
- `skydiag_crash_object_e2e_tests`: 헬퍼 함수가 이 프로세스의 실제 쓰기 데이터에서 레지스터·스택이 가리킨 잠금 창과 헤더만 담고(코드·힙·숫자 제외), 지연 스텁에 있는 게임 스레드만 고르는지.
- 보관 덤프 19개(크래시 8, 프리징 10, 수동 1)를 한국어·영어로 main 분석기와 나란히 재분석: 출력 변화 없음.
- 실제 게임 수동 캡처(1.6.1170, 2026-10-23): 헬퍼 로그 `Lock memory: threads=4 pointers=4 ranges=3 bytes=4224 time_us=375`. 덤프에 EXE 헤더 첫 0x1000바이트가 들어 있습니다. 헤더는 잠금 창을 하나라도 담을 때 함께 담는 것으로, 이 기능 전의 수동 캡처(2026-10-18)에는 없습니다.
- 1.5.97 바이너리 대조(2026-10-29, 복호화한 1.5.97 `SkyrimSE.exe`를 `.pdata`와 바이트로 직접 읽음):
  - 세 항목의 스택 위치는 모두 호출 바로 뒤입니다. 첫 위치는 스핀 잠금 대기(`+0x132BD0`)와 쓰기 잠금 대기(`+0xC07350`) 함수가 `KERNEL32!Sleep`을 부른 뒤입니다.
  - 각 위치 바로 앞의 호출은 앞 위치가 든 함수를 부릅니다. 예외 둘은 중간 프레임을 건너뛴 곳으로, 매칭은 사이의 다른 프레임을 허용합니다: 자기 교착의 `+0x27DAB3` → `+0x6D4A31`(Context의 `…`), ABBA 스레드 1의 `+0x6025FB` → `+0x6EF480`(`+0x6EF480` 앞의 호출 대상 `+0x603640`은 `+0x61C6E0`으로 점프하고, 그 함수에는 `+0x602410`을 직접 부르는 곳 없이 가상 함수 호출만 있음).
  - 잠금 세 개는 `.data` 안이고, 그 주소를 `lea`로 쓰는 함수가 스택의 함수와 맞습니다. `0x2F3B8E8`: `+0x6D9720`(대기 위치 `+0x6D9750` 바로 앞에서). `0x2F3B8F0`: `+0x6D3480`, `+0x6D3530`, `+0x6D3FF0` 등. `0x2EFF8E0`: `+0x296C00` 하나뿐이고, ABBA의 스레드 1은 이 함수의 `+0x296C3D`에서 이 잠금을 기다리며 스레드 0도 이 함수 안(`+0x2971A4`)에 있습니다.

## References
- GarrixWong/skyrim-freeze-fix README, 622866b: https://github.com/GarrixWong/skyrim-freeze-fix/blob/622866bf90bbb3c99f6604acc9ca558d01ffb062/README.md (Nexus 160704)
- GoAhead-at/FreezeLogger case study, c8fed49: `docs/case-study/appendix-A-evidence.md`, `docs/case-study/06-root-cause.md`, `skyrim-freeze-fix/` (Nexus 180884)
- CommonLibSSE-NG `include/RE/B/BSAtomic.h`
- `shared/SkyrimDiagGameLocks.h`, `helper/src/HangObjectMemory.cpp`, `dump_tool/src/GameLockWait.cpp`, `dump_tool/src/SignatureDatabase.cpp`, `dump_tool/data/crash_signatures.json`
- ADR-0009, ADR-0010 부록(2026-10-22), ADR-0015
