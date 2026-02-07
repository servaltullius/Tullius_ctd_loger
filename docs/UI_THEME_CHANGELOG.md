# 툴리우스 CTD 로거 UI 테마 변경사항

## 개요
스카이림/엘더스크롤 세계관에 어울리는 다크 판타지 테마를 적용했습니다.

## v4 – 연구 기반 리듬/가독성 개선 (WinUI 3)

v3 구조는 유지하면서, 실제 진단 툴 패턴(overview-first + drill-down)과 Fluent 가이드에 맞춰
**단조로움 완화 + 정보 계층 강화 + 좌측 치우침/아이콘 전용 문제 해소**를 반영했습니다.

### 핵심 변경

#### 1) 네비게이션 안정화
- `NavigationView`를 `PaneDisplayMode="Left"`로 고정
- `IsPaneToggleButtonVisible="False"`로 설정해 아이콘-only 상태로 축소되는 문제 방지
- 좌측 내비 라벨을 항상 유지해 초보 사용자 탐색 부담 감소

#### 2) 분석 워크플로 카드 추가
- Analyze 섹션에 3단계 워크플로 카드 도입:
  1. Dump 선택
  2. Analyze 실행
  3. Suspect → Evidence 순서로 triage
- 사용자가 "다음에 뭘 해야 하는지"를 즉시 이해할 수 있도록 구조화

#### 3) Crash Summary에 Triage 카드 추가
- 요약 상단에 4개 Quick 카드 추가:
  - Primary suspect
  - Confidence
  - Next actions
  - Blackbox events
- 분석 결과 바인딩 시 실시간으로 값 갱신 (`RenderSummary`, `RenderAdvancedArtifacts`)

#### 4) 단조로운 평면감 개선
- `Window / Pane / Hero / Section / Elevated` 배경을 분리한 다층 그라디언트 토큰 적용
- 섹션 타이틀 하단 accent bar 추가
- Evidence/Resources 영역 Expander를 카드 래핑해 정보 블록 경계 강화

#### 5) 한/영 로컬라이즈 확장
- 새로 추가된 워크플로/Quick 카드 텍스트를 기존 `T(en, ko)` 경로에 연결

### 참고한 설계 기준(요약)
- Fluent layout: spacing으로 계층/리듬을 만든다
- NavigationView: 카테고리가 5~10개일 때 Left 모드가 적합
- Grafana/Sentry: 상단에서 빠른 triage 지표를 제시하고, 상세 근거로 drill-down

## v3 – 모던 다크 판타지 리디자인 (WinUI 3 + Mica)

이전의 플랫 카드 레이아웃에서 **WinUI 3 네이티브 패턴** 기반의 모던 UI로 전면 재설계.
Mica 백드롭, NavigationView, Expander 등 Fluent Design 핵심 요소를 적용.

### 구조적 변경

#### NavigationView 도입
하나의 긴 스크롤 → **5개 섹션으로 분리된 좌측 사이드바 내비게이션**:
1. **Analyze** – 파일 선택, 분석 실행
2. **Crash Summary** – 요약, 스탯 카드, 원인 후보, 권장 조치
3. **Evidence** – 콜스택/근거/리소스 (Expander로 접기/펼치기)
4. **Events** – 블랙박스 이벤트 타임라인
5. **Report** – WCT JSON + 전체 리포트

#### Mica 백드롭
- `SystemBackdrop = new MicaBackdrop()` 적용
- 바탕화면 색상이 은은하게 비치는 Windows 11 네이티브 글래스 효과
- Windows 10에서는 자동 다크 폴백

#### Expander 컨트롤
- Evidence 섹션의 콜스택/근거/리소스를 접기/펼치기 가능
- 기본 상태: 콜스택만 펼침, 나머지 접힘

#### 신뢰도 Pill 배지
- 텍스트 기반 신뢰도 → **프로스트 블루 pill 배지** 형태
- 원인 후보, 근거 항목에 적용

#### 타이포그래피
- 본문: Georgia → **Segoe UI** (시스템 기본, 가독성 향상)
- 히어로 타이틀만 Palatino Linotype 유지 (로어 브랜딩)
- 코드: Cascadia Mono 유지

#### 기타
- CornerRadius: 6 → **8** (Fluent Design 표준)
- 카드 간 깊이 차이: `CardBackgroundBrush` (90%) vs `ElevatedBackgroundBrush` (94%)
- 분석 완료 시 자동으로 Crash Summary 페이지 이동
- "Analyze now" 버튼에 `AccentButtonStyle` 적용

### 컬러 팔레트

#### 배경 (Background)
| 이름 | HEX | 용도 |
|------|-----|------|
| Mica | 시스템 관리 | 창 배경 (바탕화면 반영) |
| CardBackground | #E6161A2E | 카드 배경 (90% 불투명) |
| ElevatedBackground | #F0202540 | 높은 카드 (94% 불투명) |
| InputBackground | #0F1225 | 입력 필드 |

#### 텍스트 (Text)
| 이름 | HEX | 용도 |
|------|-----|------|
| HeaderBrush | #D4A853 | 헤더/제목 (드래곤 골드) |
| BodyTextBrush | #E2E8F0 | 본문 (서리 화이트) |
| SubtleTextBrush | #8B95A8 (70%) | 보조 텍스트 |

#### 액센트 (Accents)
| 이름 | HEX | 용도 |
|------|-----|------|
| AccentBrush | #5BC0BE | 서리 마법 블루 |
| ConflictBrush | #C75050 | 충돌 표시 |
| CardBorderBrush | #D4A853 (20%) | 금빛 윤곽 |

---

## v2 – 다크 판타지 (색상 교체)

양피지/베이지 톤 → 다크 판타지 컬러로 교체. 구조는 플랫 카드 유지.

---

## v1 – 초기 노르딕 테마 (레거시)

양피지 베이지 + 브론즈 악센트의 따뜻한 노르딕 테마.

## Windows 빌드 방법

```powershell
cd C:\Users\kdw73\Tullius_ctd_loger
scripts\build-win.cmd
python scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip
```

## 참고사항
- Windows 11 22H2+: Mica 효과 완전 지원
- Windows 10/11 이전 빌드: 자동 다크 폴백
- RequestedTheme="Dark" 설정으로 모든 WinUI 컨트롤 다크 모드 적용
