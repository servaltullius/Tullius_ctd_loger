# 툴리우스 CTD 로거 UI 테마 변경사항

## 개요
스카이림/엘더스크롤 세계관에 어울리는 다크 판타지 테마를 적용했습니다.

## 적용된 컬러 팔레트

### 배경 색상 (Background)
| 이름 | HEX | 용도 |
|------|-----|------|
| BG_BASE | #1a1b2e | 메인 윈도우 배경 (딥 인디고) |
| BG_CARD | #1e293b | 카드/패널 배경 (다크 슬레이트) |
| BG_INPUT | #0f172a | 입력 필드 배경 (딤 블루) |

### 텍스트 색상 (Text)
| 이름 | HEX | 용도 |
|------|-----|------|
| TEXT_PRIMARY | #e2e8f0 | 주요 텍스트 (아이스 화이트) |
| TEXT_SECONDARY | #94a3b8 | 보조 텍스트 |
| TEXT_MUTED | #64748b | 비활성/힌트 텍스트 |

### 액센트 색상 (Accents)
| 이름 | HEX | 용도 |
|------|-----|------|
| ACCENT_GOLD | #d4a853 | 드래곤 골드 (강조) |
| ACCENT_AMBER | #f59e0b | 앰버 (경고) |
| ACCENT_ICE | #5bc0be | 프로스트 블루 (정보) |
| ACCENT_CYAN | #8fd1ff | 시안 (하이라이트) |

### 신뢰도 표시 색상 (Confidence Badges)
| 수준 | 배경 | 전경 |
|------|------|------|
| 높음 | #14532d (딥 그린) | #86efac (라이트 그린) |
| 중간 | #78350f (딥 앰버) | #fbbf24 (라이트 앰버) |
| 낮음 | #7f1d1d (딥 레드) | #fca5a5 (라이트 레드) |

## 주요 변경 사항

### 1. 다크 모드 지원
- Windows 10/11 DWM 다크 모드 활성화 (`DWMWA_USE_IMMERSIVE_DARK_MODE`)
- 타이틀 바 다크 테마 적용

### 2. UI 컨트롤 테마
- **Tab Control**: DarkMode 테마 적용
- **ListView**: 다크 배경 + 아이스 화이트 텍스트
- **Edit Control**: 다크 입력 필드 테마
- **Button**: DarkMode 테마 적용

### 3. 시각적 개선
- Confidence badge 색상을 다크 테마에 맞게 조정
- Row height 및 패딩 최적화
- Grid lines 제거로 모던한 룩 적용

## Windows 빌드 방법

```powershell
# Windows에서 실행
cd C:\Users\kdw73\Tullius_ctd_loger

# 빌드
scripts\build-win.cmd

# 패키징
python scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip
```

## 참고사항
- Windows 10 1809 (build 17763) 이상에서 다크 타이틀 바 정상 작동
- 이전 Windows 버전에서는 다크 테마가 제한적으로 적용될 수 있음
