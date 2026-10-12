# ADR-0012: Memory Snapshot at Capture Time

## Status
Accepted

## Context
메모리 부족은 Skyrim 모드 환경의 흔한 사고 원인입니다.

- 시스템 커밋이 한도에 닿으면 메모리 할당이 실패하고 게임이 튕깁니다.
- 게임이 VRAM 예산을 넘기면 Windows가 텍스처를 시스템 메모리로 밀어내서 끊김, 프리징, 드라이버 크래시가 납니다.

Crash Logger는 로그의 SYSTEM SPECS에 `PHYSICAL MEMORY`, `PROCESS MEMORY`, `GPU MEMORY: 사용량/예산`을 적고, 커뮤니티 분석기는 이 값으로 "RAM 부족"이나 "VRAM 85% 초과"를 판정합니다. 우리 리포트에는 이런 정보가 없었습니다.

조사 결과는 다음과 같습니다.

- 보관 중인 덤프 17개 모두에 Windows(dbghelp)가 자동으로 넣은 `SystemMemoryInfoStream`(21)과 `ProcessVmCountersStream`(22)이 있습니다. 덤프를 쓰는 순간의 시스템 RAM·커밋과 게임 프로세스의 커밋·워킹셋입니다. 값은 같은 사고의 Crash Logger 로그와 일치했습니다(2026-10-07: 사용 가능 RAM 24.3GB, 게임 커밋 25,098MB, 워킹셋 14,108MB).
- VRAM은 덤프에 없습니다. 성능 카운터(PDH `GPU Process Memory`)는 첫 조회에 1~6초가 걸려 캡처 경로에 맞지 않습니다.
- `D3DKMTQueryStatistics`의 `D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP`은 다른 프로세스의 어댑터별 사용량과 예산을 0.1ms 안에 돌려줍니다. 이 값은 게임 안에서 DXGI `QueryVideoMemoryInfo`가 돌려주는 값과 같습니다. 이 PC에서 예산 22.82GB가 Crash Logger의 `GPU MEMORY: 4.83/22.82 GB`와 정확히 일치했습니다.

## Decision

1. 분석기는 덤프의 스트림 21과 22를 읽어 시스템 RAM(전체, 사용 가능), 시스템 커밋(사용, 한도), 게임 커밋과 워킹셋을 얻는다. 이전 덤프에도 들어 있어 다시 분석하면 보인다(WinUI가 기존 분석 결과를 불러오는 경우에는 다시 분석해야 한다).
2. 헬퍼는 모든 덤프(크래시, 프리징, 수동)에 사용자 스트림 `kMinidumpUserStream_GpuMemory`(`0x1474D`, JSON)를 넣는다.
   - 내용은 게임이 메모리를 쓰고 있는 어댑터별 local/non-local 사용량과 예산, 전용 메모리 크기, 어댑터 이름이다.
   - 값은 `D3DKMTQueryStatistics`로 읽는다. 함수는 실행 중에 gdi32에서 찾는다(Wine이나 구형 Windows에서 import 실패로 헬퍼가 뜨지 못하는 일을 막기 위해서다).
   - 게임 PID로 핸들을 따로 연다. PSS 스냅샷 경로도 포함된다.
   - 어댑터 수를 먼저 물어 모든 어댑터를 열고, 연 핸들은 어떤 경로로든 닫는다.
3. 분석기는 local 사용량이 가장 큰 어댑터를 게임의 GPU로 본다.
4. 압박 판정
   - 시스템 커밋이 한도의 90% 이상이거나 VRAM 사용량이 예산의 90% 이상이면 "한도에 가까웠음"(신뢰도 중간)으로 표시하고 `[메모리]` 권장 조치를 붙인다.
   - 그 밖에는 "캡처 당시 메모리"(신뢰도 낮음) 근거 항목으로 숫자만 보인다.
   - 사용 가능 RAM이 적은 것만으로는 판정하지 않는다. 페이지 파일이 받쳐 주는 동안에는 할당이 실패하지 않기 때문이다.
5. 메모리 압박은 모드를 지목하지 않는다. 후보, 버킷, 이력에 영향을 주지 않으며 다음 조치(NextAction)에서도 후보 줄보다 앞서지 않는다.
6. 출력
   - 근거 항목
   - 리포트의 `메모리:`, `GPU:` 줄
   - 요약 JSON의 `memory`(`system`, `process`, `gpu`, `commit_high`, `vram_high`)

## Consequences
- 보관 덤프 17개는 커밋이 최대 78%라 `[메모리]` 권장 조치가 붙지 않는다. 다른 결론은 바뀌지 않는다.
- VRAM은 이 변경 이후에 캡처한 덤프에만 있다.
- 값은 캡처 순간의 것이다. 덤프를 쓰는 동안 헬퍼와 dbghelp가 쓰는 메모리만큼 시스템 커밋이 조금 늘어날 수 있다.
- PSS 스냅샷으로 쓴 프리징 덤프(`EnablePssSnapshotForFreeze=1`, 기본값 꺼짐)는 스냅샷 복제본이 덤프를 쓰는 동안 시스템 커밋에 잡힌다(2GB 시험에서 +2.1GB). 그래서 이 경우 커밋 판정은 하지 않고 숫자에 "PSS 스냅샷이 복제한 게임 메모리 포함"을 붙인다. 분석기는 WCT JSON의 `pss_snapshot_used`로 알아본다.
- 압박 판정은 반올림한 백분율이 아니라 실제 비율로 한다(89.6%는 90%로 표시되지만 압박이 아니다).
- GPU 스트림의 필드 형식이 틀리면 그 필드를 없는 것으로 읽는다. 분석 전체를 실패시키지 않는다.

## Verification
- `skydiag_memory_snapshot_tests`: 합성 덤프의 스트림 21·22와 GPU 스트림, 압박 기준(실제 비율, PSS 덤프), 형식이 틀린 GPU JSON, 영어·한국어 근거와 권장 조치, 리포트와 JSON, 다음 조치 우선순위.
- `skydiag_memory_snapshot_e2e_tests`: 헬퍼 JSON을 분석기가 그대로 읽는지(모든 환경), 실제 헬퍼 덤프의 RAM 전체·커밋 한도가 `GlobalMemoryStatusEx`와 같은지, 실제 GPU가 있으면 텍스처 128MB가 VRAM 사용량에 잡히는지.
- 보관 덤프 17개 재분석: rc2 분석기 대비 메모리 근거 항목과 리포트 줄 외에는 바뀌지 않음.
- 90%는 보수적인 기준이다. 커뮤니티 분석기의 85%보다 높게 잡아 정상 환경의 경고를 줄인다.
