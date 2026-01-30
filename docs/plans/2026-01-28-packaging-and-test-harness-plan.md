# Packaging + Test Harness Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** MO2-friendly 배포 ZIP을 자동 생성하고, 실게임에서 CTD/무한로딩/행(hang) 캡처를 빠르게 검증할 수 있는 “테스트 트리거(옵션)”를 제공한다.

**Architecture:** 배포는 `scripts/package.py`가 빌드 산출물(`SkyrimDiag.dll`, `SkyrimDiagHelper.exe`)과 기본 ini를 모아 MO2에서 바로 설치 가능한 폴더 구조로 ZIP을 만든다. 검증은 플러그인에 “테스트 핫키(기본 OFF)”를 추가해 의도적으로 crash/hang을 유발해 Helper가 덤프/WCT를 생성하는지 확인한다.

**Tech Stack:** CMake/vcpkg, CommonLibSSE-NG, Win32 API, Python(Zip 생성)

---

### Task 1: 배포 ZIP 패키저 추가

**Files:**
- Create: `scripts/package.py`
- Modify: `README.md:1`

**Step 1: 패키징 스펙 정의**
- ZIP 내부 구조(예):
  - `SKSE/Plugins/SkyrimDiag.dll`
  - `SKSE/Plugins/SkyrimDiag.pdb` (있으면)
  - `SKSE/Plugins/SkyrimDiag.ini`
  - `SKSE/Plugins/SkyrimDiagHelper.exe`
  - `SKSE/Plugins/SkyrimDiagHelper.ini`

**Step 2: 스크립트 구현**
- 입력: `--build-dir`(기본 `build`), `--bin-dir`(기본 `<build>/bin`), `--out`(기본 `dist/SkyrimDiag_<ts>.zip`)
- 동작: 필요한 파일 존재 확인 → 임시 디렉터리에 구조 생성 → `zipfile`로 압축

**Step 3: 사용법 문서화**
- `README.md`에 `python scripts/package.py ...` 예시 추가

---

### Task 2: “강제 crash/hang” 테스트 트리거(옵션) 추가

**Files:**
- Modify: `dist/SkyrimDiag.ini:1`
- Modify: `plugin/src/PluginMain.cpp:1`

**Step 1: 설정 추가(기본 OFF)**
- `EnableTestHotkeys=0`
- `TestCrashHotkey=...` (고정 단축키로 시작: Ctrl+Shift+F10)
- `TestHangHotkey=...` (Ctrl+Shift+F11)

**Step 2: 플러그인에서 입력 감지**
- 백그라운드 스레드가 `GetAsyncKeyState()`로 단축키를 폴링(저부하)
- 트리거 시 `SKSE::TaskInterface::AddTask()`로 메인스레드에서:
  - crash: null ptr write로 access violation 유발
  - hang: 무한 sleep/루프

**Step 3: 안전장치**
- 트리거는 “세션당 1회”만 동작(atomic flag)
- 기본 OFF 유지

---

### Task 3: 테스트/사용 가이드 추가

**Files:**
- Modify: `README.md:1`
- Modify: `docs/plans/2026-01-28-skyrimdiag-mvp-a-design.md:1`

**Step 1: MO2 설치 가이드**
- Helper 자동 실행(플러그인에서 `AutoStartHelper=1`) + 수동 캡처 핫키 안내

**Step 2: 검증 시나리오**
- (1) 정상 로딩 1회 → `SkyrimDiag_LoadStats.json` 생성 확인
- (2) Ctrl+Shift+F12 수동 캡처로 덤프/WCT 생성 확인
- (3) (옵션) EnableTestHotkeys=1 후 Ctrl+Shift+F10/F11로 crash/hang 유발 → 덤프 생성 확인

---

### Task 4: 기본 검증

**Files:** (none)

**Step 1: CMake configure/build**
- Linux 호스트에서는 Windows 타깃이 스킵되는지 확인(현재 동작)
- Windows에서 `cmake --preset default` + `cmake --build`가 성공해야 “완성”에 가까움

