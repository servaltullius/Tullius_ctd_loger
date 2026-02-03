#include "GuiApp.h"
#include "DumpToolConfig.h"
#include "OutputWriter.h"
#include "Utf.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// ============================================================================
// Skyrim/Elder Scrolls Dark Fantasy Theme Color Palette
// ============================================================================
namespace theme {
  // Background colors (Deep Indigo/Midnight)
  constexpr COLORREF BG_BASE = RGB(26, 27, 46);        // #1a1b2e - Main window
  constexpr COLORREF BG_CARD = RGB(30, 41, 59);        // #1e293b - Card/panel
  constexpr COLORREF BG_INPUT = RGB(15, 23, 42);       // #0f172a - Input fields
  
  // Text colors (Ice/Frost white)
  constexpr COLORREF TEXT_PRIMARY = RGB(226, 232, 240);   // #e2e8f0 - Primary text
  constexpr COLORREF TEXT_SECONDARY = RGB(148, 163, 184); // #94a3b8 - Secondary text
  constexpr COLORREF TEXT_MUTED = RGB(100, 116, 139);     // #64748b - Muted text
  
  // Accent colors (Dragonfire & Frost)
  constexpr COLORREF ACCENT_GOLD = RGB(212, 168, 83);     // #d4a853 - Dragon gold
  constexpr COLORREF ACCENT_AMBER = RGB(245, 158, 11);    // #f59e0b - Amber
  constexpr COLORREF ACCENT_ICE = RGB(91, 192, 190);      // #5bc0be - Frost blue
  constexpr COLORREF ACCENT_CYAN = RGB(143, 209, 255);    // #8fd1ff - Cyan
  
  // Status/Confidence colors
  constexpr COLORREF CONF_HIGH_BG = RGB(20, 83, 45);      // #14532d - Deep green
  constexpr COLORREF CONF_HIGH_FG = RGB(134, 239, 172);   // #86efac - Light green
  constexpr COLORREF CONF_MED_BG = RGB(120, 53, 15);      // #78350f - Deep amber
  constexpr COLORREF CONF_MED_FG = RGB(251, 191, 36);     // #fbbf24 - Light amber
  constexpr COLORREF CONF_LOW_BG = RGB(127, 29, 29);      // #7f1d1d - Deep red
  constexpr COLORREF CONF_LOW_FG = RGB(252, 165, 165);    // #fca5a5 - Light red
  
  // Border/Divider
  constexpr COLORREF BORDER = RGB(51, 65, 85);            // #334155
}

namespace skydiag::dump_tool {
namespace {

constexpr wchar_t kWndClassName[] = L"SkyrimDiagDumpToolViewer";

enum : int
{
  kIdTab = 100,
  kIdSummaryEdit = 110,
  kIdEvidenceList = 120,
  kIdRecoList = 121,
  kIdEvidenceSplitter = 122,
  kIdEventsList = 130,
  kIdEventsFilterEdit = 131,
  kIdResourcesList = 135,
  kIdWctEdit = 140,
  kIdBtnCopy = 200,
  kIdBtnOpenFolder = 201,
  kIdBtnOpenDump = 202,
  kIdBtnCopyChecklist = 203,
  kIdBtnLanguage = 204,
};

struct AppState
{
  GuiOptions guiOpt{};
  AnalyzeOptions analyzeOpt{};
  AnalysisResult r{};

  HWND hwnd = nullptr;
  HWND tab = nullptr;

  HFONT uiFont = nullptr;
  HFONT uiFontSemibold = nullptr;
  HFONT uiFontMono = nullptr;

  HBRUSH bgBaseBrush = nullptr;
  HBRUSH bgCardBrush = nullptr;

  HIMAGELIST rowHeightImages = nullptr;

  HWND summaryEdit = nullptr;
  HWND evidenceList = nullptr;
  HWND evidenceSplitter = nullptr;
  HWND recoList = nullptr;
  HWND eventsList = nullptr;
  HWND eventsFilterEdit = nullptr;
  HWND resourcesList = nullptr;
  HWND wctEdit = nullptr;

  int evidenceSplitPercent = 55;
  bool evidenceSplitDragging = false;

  std::wstring lastError;
};

UINT GetDpiForWindowCompat(HWND hwnd)
{
  static auto p = reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (p) {
    return p(hwnd);
  }
  return 96;
}

int ScalePx(HWND hwnd, int px96)
{
  const UINT dpi = hwnd ? GetDpiForWindowCompat(hwnd) : 96;
  return MulDiv(px96, static_cast<int>(dpi), 96);
}

const wchar_t* LText(i18n::Language lang, const wchar_t* en, const wchar_t* ko)
{
  return (lang == i18n::Language::kEnglish) ? en : ko;
}

HFONT CreateUiFont(HWND hwnd)
{
  const UINT dpi = hwnd ? GetDpiForWindowCompat(hwnd) : 96;
  const int height = -MulDiv(9, static_cast<int>(dpi), 72);
  return CreateFontW(
    height,
    0,
    0,
    0,
    FW_NORMAL,
    FALSE,
    FALSE,
    FALSE,
    DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE,
    L"Segoe UI");
}

HFONT CreateUiFontSemibold(HWND hwnd)
{
  const UINT dpi = hwnd ? GetDpiForWindowCompat(hwnd) : 96;
  const int height = -MulDiv(9, static_cast<int>(dpi), 72);
  return CreateFontW(
    height,
    0,
    0,
    0,
    FW_SEMIBOLD,
    FALSE,
    FALSE,
    FALSE,
    DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE,
    L"Segoe UI");
}

HFONT CreateUiFontMono(HWND hwnd)
{
  const UINT dpi = hwnd ? GetDpiForWindowCompat(hwnd) : 96;
  const int height = -MulDiv(9, static_cast<int>(dpi), 72);
  return CreateFontW(
    height,
    0,
    0,
    0,
    FW_NORMAL,
    FALSE,
    FALSE,
    FALSE,
    DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE,
    L"Consolas");
}

void ApplyFont(HWND w, HFONT f)
{
  if (!w || !f) {
    return;
  }
  SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
}

void ApplyExplorerTheme(HWND w)
{
  if (!w) {
    return;
  }
  SetWindowTheme(w, L"Explorer", nullptr);
}

int ClampInt(int v, int lo, int hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void Layout(AppState* st);

void ApplyListViewModernStyle(HWND lv, bool dark = true)
{
  if (!lv) {
    return;
  }

  // Remove gridlines for a more modern look.
  DWORD ex = ListView_GetExtendedListViewStyle(lv);
  ex &= ~static_cast<DWORD>(LVS_EX_GRIDLINES);
  ex |= (LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ListView_SetExtendedListViewStyle(lv, ex);

  if (dark) {
    // Dark theme background
    ListView_SetBkColor(lv, theme::BG_CARD);
    ListView_SetTextBkColor(lv, theme::BG_CARD);
    ListView_SetTextColor(lv, theme::TEXT_PRIMARY);
    SetWindowTheme(lv, L"DarkMode_Explorer", nullptr);
  } else {
    // White "card" background.
    ListView_SetBkColor(lv, RGB(255, 255, 255));
    ListView_SetTextBkColor(lv, RGB(255, 255, 255));
  }
}

// Enable Windows 10/11 dark title bar and immersive dark mode
void EnableDarkMode(HWND hwnd)
{
  // Windows 10 1809+ (build 17763)
  constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
  
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

  // Windows 11: rounded corners (best-effort; ignore failures on older builds).
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
  typedef enum DWM_WINDOW_CORNER_PREFERENCE {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3,
  } DWM_WINDOW_CORNER_PREFERENCE;
#endif
  const DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
  
  // Set dark mode for the window theme
  SetWindowTheme(hwnd, L"DarkMode", nullptr);
}

// Apply dark theme to buttons
void ApplyDarkButton(HWND btn)
{
  if (!btn) return;
  SetWindowTheme(btn, L"DarkMode", nullptr);
}

// Apply dark theme to edit controls
void ApplyDarkEdit(HWND edit)
{
  if (!edit) return;
  SetWindowTheme(edit, L"DarkMode", nullptr);
}

// Apply dark theme to tab control
void ApplyDarkTab(HWND tab)
{
  if (!tab) return;
  SetWindowTheme(tab, L"DarkMode", nullptr);
}

void ApplyEditPadding(HWND edit, int px)
{
  if (!edit) {
    return;
  }
  SendMessageW(
    edit,
    EM_SETMARGINS,
    EC_LEFTMARGIN | EC_RIGHTMARGIN,
    MAKELPARAM(px, px));
}

void DrawModernButton(const AppState* st, const DRAWITEMSTRUCT* dis, bool isPrimary)
{
  if (!st || !dis) {
    return;
  }

  wchar_t text[256]{};
  GetWindowTextW(dis->hwndItem, text, static_cast<int>(sizeof(text) / sizeof(text[0])));

  const bool isDisabled = (dis->itemState & ODS_DISABLED) != 0;
  const bool isPressed = (dis->itemState & ODS_SELECTED) != 0;
  const bool isHot = (dis->itemState & ODS_HOTLIGHT) != 0;
  const bool isFocused = (dis->itemState & ODS_FOCUS) != 0;

  COLORREF bg = theme::BG_INPUT;
  COLORREF border = theme::BORDER;
  COLORREF fg = theme::TEXT_PRIMARY;

  if (isPrimary) {
    // Primary: "gold" action button (Skyrim vibe, but still clean).
    bg = RGB(88, 65, 25);
    border = theme::ACCENT_GOLD;
    fg = RGB(255, 255, 255);
    if (isHot) {
      bg = RGB(107, 79, 31);
    }
    if (isPressed) {
      bg = RGB(70, 52, 20);
    }
  } else {
    // Secondary buttons.
    bg = theme::BG_INPUT;
    border = theme::BORDER;
    fg = theme::TEXT_PRIMARY;
    if (isHot) {
      bg = RGB(51, 65, 85);
    }
    if (isPressed) {
      bg = theme::BG_CARD;
    }
  }

  if (isDisabled) {
    bg = theme::BG_CARD;
    border = theme::BORDER;
    fg = theme::TEXT_MUTED;
  }

  HDC hdc = dis->hDC;
  RECT rc = dis->rcItem;

  // Background.
  HBRUSH bgBrush = CreateSolidBrush(bg);
  HPEN borderPen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
  HGDIOBJ oldPen = SelectObject(hdc, borderPen);

  const int radius = ScalePx(st->hwnd, 8);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

  // Text.
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, fg);
  DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  // Focus ring.
  if (isFocused && !isPressed) {
    RECT frc = rc;
    InflateRect(&frc, -2, -2);
    DrawFocusRect(hdc, &frc);
  }

  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(borderPen);
  DeleteObject(bgBrush);
}

LRESULT HandleEvidenceCustomDraw(AppState* st, LPNMLVCUSTOMDRAW cd)
{
  if (!st || !cd) {
    return CDRF_DODEFAULT;
  }

  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
      return CDRF_NOTIFYSUBITEMDRAW;
    case (CDDS_SUBITEM | CDDS_ITEMPREPAINT): {
      const int col = cd->iSubItem;
      const bool isSelected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
      const bool isHot = (cd->nmcd.uItemState & CDIS_HOT) != 0;

      // Subtle row striping helps readability in long evidence lists.
      COLORREF rowBg = ((cd->nmcd.dwItemSpec % 2) == 0) ? theme::BG_CARD : theme::BG_INPUT;
      if (isSelected) {
        rowBg = RGB(30, 58, 138);  // deep blue selection
      } else if (isHot) {
        rowBg = RGB(51, 65, 85);  // hover (slightly brighter than BG_CARD)
      }

      if (col == 0) {
        wchar_t text[32]{};
        ListView_GetItemText(cd->nmcd.hdr.hwndFrom, static_cast<int>(cd->nmcd.dwItemSpec), 0, text, 32);

        // Dark theme confidence badge colors (Dragonfire theme)
        const auto level = static_cast<i18n::ConfidenceLevel>(cd->nmcd.lItemlParam);
        COLORREF fg = theme::TEXT_SECONDARY;
        COLORREF bg = theme::BG_INPUT;
        COLORREF border = theme::BORDER;
        if (level == i18n::ConfidenceLevel::kHigh) {
          fg = theme::CONF_HIGH_FG;
          bg = theme::CONF_HIGH_BG;
          border = fg;
        } else if (level == i18n::ConfidenceLevel::kMedium) {
          fg = theme::CONF_MED_FG;
          bg = theme::CONF_MED_BG;
          border = fg;
        } else if (level == i18n::ConfidenceLevel::kLow) {
          fg = theme::CONF_LOW_FG;
          bg = theme::CONF_LOW_BG;
          border = fg;
        }

        // Theme'd listviews on modern Windows can ignore clrTextBk for subitems.
        // Draw the confidence cell ourselves to guarantee the badge colors are visible.
        HDC hdc = cd->nmcd.hdc;
        RECT rc{};
        if (ListView_GetSubItemRect(
              cd->nmcd.hdr.hwndFrom,
              static_cast<int>(cd->nmcd.dwItemSpec),
              /*subItem=*/0,
              LVIR_BOUNDS,
              &rc)) {
          // For subItem=0, LVIR_BOUNDS covers the whole row. Clamp to the first column width.
          const int colW = ListView_GetColumnWidth(cd->nmcd.hdr.hwndFrom, 0);
          rc.right = rc.left + colW;
        } else {
          rc = cd->nmcd.rc;
        }

        HFONT oldFont = nullptr;
        if (st->uiFontSemibold) {
          oldFont = static_cast<HFONT>(SelectObject(hdc, st->uiFontSemibold));
        }

        // Base cell background (striped/selection-aware).
        HBRUSH cellBg = CreateSolidBrush(rowBg);
        FillRect(hdc, &rc, cellBg);
        DeleteObject(cellBg);

        // "Pill" badge centered in the cell.
        const int len = static_cast<int>(wcslen(text));
        SIZE ts{};
        if (!GetTextExtentPoint32W(hdc, text, len, &ts)) {
          ts.cx = 0;
          ts.cy = 0;
        }

        const int padX = ScalePx(st->hwnd, 10);
        const int padY = ScalePx(st->hwnd, 4);
        const int badgeW = std::min<int>(ts.cx + padX * 2, (rc.right - rc.left) - ScalePx(st->hwnd, 8));
        const int badgeH = std::min<int>(ts.cy + padY * 2, (rc.bottom - rc.top) - ScalePx(st->hwnd, 4));

        RECT badge = rc;
        badge.left = rc.left + ((rc.right - rc.left) - badgeW) / 2;
        badge.right = badge.left + badgeW;
        badge.top = rc.top + ((rc.bottom - rc.top) - badgeH) / 2;
        badge.bottom = badge.top + badgeH;

        const int radius = ScalePx(st->hwnd, 10);
        HBRUSH badgeBrush = CreateSolidBrush(bg);
        HPEN badgePen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(hdc, badgeBrush);
        HGDIOBJ oldPen = SelectObject(hdc, badgePen);
        RoundRect(hdc, badge.left, badge.top, badge.right, badge.bottom, radius, radius);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(badgePen);
        DeleteObject(badgeBrush);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        DrawTextW(hdc, text, len, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont) {
          SelectObject(hdc, oldFont);
        }
        return CDRF_SKIPDEFAULT;
      }

      // Default text colors for other columns (dark theme).
      cd->clrText = theme::TEXT_PRIMARY;
      cd->clrTextBk = rowBg;
      return CDRF_DODEFAULT;
    }
  }

  return CDRF_DODEFAULT;
}

LRESULT CALLBACK EvidenceSplitterSubclassProc(
  HWND hwnd,
  UINT msg,
  WPARAM wParam,
  LPARAM lParam,
  UINT_PTR,
  DWORD_PTR refData)
{
  auto* st = reinterpret_cast<AppState*>(refData);
  if (!st) {
    return DefSubclassProc(hwnd, msg, wParam, lParam);
  }

  switch (msg) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
      return TRUE;

    case WM_LBUTTONDOWN:
      st->evidenceSplitDragging = true;
      SetCapture(hwnd);
      return 0;

    case WM_MOUSEMOVE: {
      if (!st->evidenceSplitDragging || GetCapture() != hwnd) {
        break;
      }

      POINT pt{};
      GetCursorPos(&pt);
      ScreenToClient(st->tab, &pt);

      RECT tabRc{};
      GetClientRect(st->tab, &tabRc);
      TabCtrl_AdjustRect(st->tab, FALSE, &tabRc);

      const int tabTop = static_cast<int>(tabRc.top);
      const int tabBottom = static_cast<int>(tabRc.bottom);
      const int h = tabBottom - tabTop;
      if (h <= 0) {
        break;
      }

      const int y = ClampInt(static_cast<int>(pt.y) - tabTop, 0, h);
      int pct = (y * 100) / h;
      pct = ClampInt(pct, 20, 80);

      if (pct != st->evidenceSplitPercent) {
        st->evidenceSplitPercent = pct;
        Layout(st);
      }
      return 0;
    }

    case WM_LBUTTONUP:
      st->evidenceSplitDragging = false;
      if (GetCapture() == hwnd) {
        ReleaseCapture();
      }
      return 0;

    case WM_CAPTURECHANGED:
      st->evidenceSplitDragging = false;
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);

      HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
      FillRect(hdc, &rc, bg);
      DeleteObject(bg);

      const int midY = (rc.top + rc.bottom) / 2;
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
      HGDIOBJ old = SelectObject(hdc, pen);
      MoveToEx(hdc, rc.left + 2, midY, nullptr);
      LineTo(hdc, rc.right - 2, midY);
      SelectObject(hdc, old);
      DeleteObject(pen);

      EndPaint(hwnd, &ps);
      return 0;
    }
  }

  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

std::wstring Quote(std::wstring_view s)
{
  std::wstring out;
  out.reserve(s.size() + 2);
  out.push_back(L'"');
  out.append(s);
  out.push_back(L'"');
  return out;
}

std::wstring WrapLines(std::wstring_view s, std::size_t width, std::wstring_view indent)
{
  if (s.empty()) {
    return {};
  }
  if (width == 0) {
    width = 80;
  }

  std::wstring out;
  out.reserve(s.size() + (s.size() / width + 1) * (indent.size() + 2));

  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t take = std::min<std::size_t>(width, s.size() - i);
    out.append(indent);
    out.append(s.substr(i, take));
    out.append(L"\r\n");
    i += take;
  }

  return out;
}

std::wstring FormatWctText(const AnalysisResult& r)
{
  const auto lang = r.language;
  if (!r.has_wct || r.wct_json_utf8.empty()) {
    return std::wstring(LText(lang, L"(No WCT data)", L"(WCT 정보 없음)"));
  }

  try {
    const auto j = nlohmann::json::parse(r.wct_json_utf8);

    const auto threadsIt = j.find("threads");
    if (threadsIt == j.end() || !threadsIt->is_array()) {
      return Utf8ToWide(r.wct_json_utf8);
    }

    struct ThreadView
    {
      std::uint32_t tid = 0;
      bool isCycle = false;
      nlohmann::json nodes;
    };

    std::vector<ThreadView> threads;
    int cycles = 0;
    for (const auto& t : *threadsIt) {
      ThreadView tv{};
      tv.tid = t.value("tid", 0u);
      tv.isCycle = t.value("isCycle", false);
      tv.nodes = t.value("nodes", nlohmann::json::array());
      if (tv.isCycle) {
        cycles++;
      }
      threads.push_back(std::move(tv));
    }

    // Cycle threads first.
    std::stable_sort(threads.begin(), threads.end(), [](const ThreadView& a, const ThreadView& b) {
      return (a.isCycle && !b.isCycle);
    });

    std::wstring out;
    out += LText(lang, L"WCT (Wait Chain) Overview\r\n", L"WCT (Wait Chain) 보기\r\n");
    out += L"- threads=" + std::to_wstring(threads.size()) + L"\r\n";
    out += L"- isCycle=true threads=" + std::to_wstring(cycles) + L"\r\n";
    out += LText(
      lang,
      L"- isCycle=true suggests a likely deadlock.\r\n",
      L"- isCycle=true면 데드락 가능성이 큽니다.\r\n");
    out += LText(
      lang,
      L"- Use this tab to diagnose freezes/infinite loading.\r\n",
      L"- 이 화면은 '프리징/무한로딩' 진단용입니다.\r\n");
    out += L"\r\n";

    const std::size_t maxThreads = 20;
    const std::size_t showN = std::min<std::size_t>(threads.size(), maxThreads);
    for (std::size_t i = 0; i < showN; i++) {
      const auto& t = threads[i];
      out += L"Thread ";
      out += std::to_wstring(t.tid);
      out += L"  (isCycle=";
      out += t.isCycle ? L"true" : L"false";
      out += L")\r\n";

      if (!t.nodes.is_array() || t.nodes.empty()) {
        out += LText(lang, L"  (no nodes)\r\n\r\n", L"  (nodes 없음)\r\n\r\n");
        continue;
      }

      const std::size_t maxNodes = 12;
      const std::size_t nodeN = std::min<std::size_t>(t.nodes.size(), maxNodes);
      for (std::size_t ni = 0; ni < nodeN; ni++) {
        const auto& n = t.nodes[ni];
        const auto type = n.value("objectType", 0u);
        const auto status = n.value("objectStatus", 0u);
        const std::wstring name = Utf8ToWide(n.value("objectName", std::string{}));
        const bool isThreadNode = (type == 8u);  // WctThreadType (Windows WCT)

        out += L"  ";
        out += std::to_wstring(ni);
        out += L") type=";
        out += std::to_wstring(type);
        out += L" status=";
        out += std::to_wstring(status);
        out += L"\r\n";

        if (!isThreadNode && !name.empty()) {
          // Wrap-friendly, truncate very long names.
          std::wstring nm = name;
          nm.erase(std::remove(nm.begin(), nm.end(), L'\r'), nm.end());
          nm.erase(std::remove(nm.begin(), nm.end(), L'\n'), nm.end());
          if (nm.size() > 200) {
            nm.resize(200);
            nm += L"...";
          }
          out += L"     name:\r\n";
          out += WrapLines(nm, 100, L"       ");
        }

        const auto thIt = n.find("thread");
        if (thIt != n.end() && thIt->is_object()) {
          const auto pid = thIt->value("processId", 0u);
          const auto tid = thIt->value("threadId", 0u);
          const auto waitTime = thIt->value("waitTime", 0u);
          const auto csw = thIt->value("contextSwitches", 0u);
          out += L"     thread: pid=" + std::to_wstring(pid) + L" tid=" + std::to_wstring(tid) + L" waitTime=" +
            std::to_wstring(waitTime) + L"ms cs=" + std::to_wstring(csw) + L"\r\n";
        }
      }
      if (t.nodes.size() > nodeN) {
        out += LText(lang, L"  ... (", L"  ... (노드 ");
        out += std::to_wstring(t.nodes.size() - nodeN);
        out += LText(lang, L" nodes omitted)\r\n", L"개 생략)\r\n");
      }
      out += L"\r\n";
    }
    if (threads.size() > showN) {
      out += LText(lang, L"... (", L"... (스레드 ");
      out += std::to_wstring(threads.size() - showN);
      out += LText(lang, L" threads omitted)\r\n", L"개 생략)\r\n");
    }

    return out;
  } catch (...) {
    return Utf8ToWide(r.wct_json_utf8);
  }
}

std::wstring FormatSummaryText(const AnalysisResult& r)
{
  const auto join = [](const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep) -> std::wstring {
    if (items.empty() || maxN == 0) {
      return {};
    }
    const std::size_t n = std::min<std::size_t>(items.size(), maxN);
    std::wstring out;
    for (std::size_t i = 0; i < n; i++) {
      if (i > 0) {
        out += sep;
      }
      out += items[i];
    }
    if (items.size() > n) {
      out += sep;
      out += L"...";
    }
    return out;
  };

  const auto lang = r.language;
  std::wstring s;
  s += LText(lang, L"Conclusion\r\n", L"결론\r\n");
  s += L"- ";
  s += r.summary_sentence.empty() ? std::wstring(LText(lang, L"(none)", L"(없음)")) : r.summary_sentence;
  s += L"\r\n\r\n";

  s += LText(lang, L"Basic info\r\n", L"기본 정보\r\n");
  s += L"- Dump: ";
  s += r.dump_path;
  s += L"\r\n";

  s += L"- Module+Offset: ";
  s += r.fault_module_plus_offset.empty() ? L"(unknown)" : r.fault_module_plus_offset;
  s += L"\r\n";

  if (!r.inferred_mod_name.empty()) {
    s += L"- ";
    s += LText(lang, L"Inferred mod", L"추정 모드");
    s += L": ";
    s += r.inferred_mod_name;
    s += L"\r\n";
  }

  if (!r.crash_logger_log_path.empty()) {
    s += L"- ";
    s += LText(lang, L"Crash Logger log", L"Crash Logger 로그");
    s += L": ";
    s += r.crash_logger_log_path;
    s += L"\r\n";
  }

  if (!r.crash_logger_top_modules.empty()) {
    s += L"- ";
    s += LText(lang, L"Crash Logger top modules", L"Crash Logger 상위 모듈");
    s += L": ";
    s += join(r.crash_logger_top_modules, 6, L", ");
    s += L"\r\n";
  }

  if (!r.crash_logger_cpp_exception_type.empty() ||
      !r.crash_logger_cpp_exception_info.empty() ||
      !r.crash_logger_cpp_exception_throw_location.empty() ||
      !r.crash_logger_cpp_exception_module.empty()) {
    std::vector<std::wstring> parts;
    if (!r.crash_logger_cpp_exception_type.empty()) {
      parts.push_back(L"Type: " + r.crash_logger_cpp_exception_type);
    }
    if (!r.crash_logger_cpp_exception_info.empty()) {
      parts.push_back(L"Info: " + r.crash_logger_cpp_exception_info);
    }
    if (!r.crash_logger_cpp_exception_throw_location.empty()) {
      parts.push_back(L"Throw: " + r.crash_logger_cpp_exception_throw_location);
    }
    if (!r.crash_logger_cpp_exception_module.empty()) {
      parts.push_back(L"Module: " + r.crash_logger_cpp_exception_module);
    }
    s += L"- ";
    s += LText(lang, L"Crash Logger C++ exception", L"Crash Logger C++ 예외");
    s += L": ";
    s += join(parts, 8, L" | ");
    s += L"\r\n";
  }

  if (!r.suspects.empty()) {
    s += L"- ";
    s += LText(lang, L"Stack scan candidates (Top 5)", L"스택 스캔 후보(Top 5)");
    s += L":\r\n";
    const std::size_t n = std::min<std::size_t>(r.suspects.size(), 5);
    for (std::size_t i = 0; i < n; i++) {
      const auto& sus = r.suspects[i];
      s += L"  - [";
      s += sus.confidence.empty() ? std::wstring(i18n::ConfidenceLabel(lang, sus.confidence_level)) : sus.confidence;
      s += L"] ";
      if (!sus.inferred_mod_name.empty()) {
        s += sus.inferred_mod_name;
        s += L" (";
        s += sus.module_filename;
        s += L")";
      } else {
        s += sus.module_filename.empty() ? L"(unknown)" : sus.module_filename;
      }
      s += L" score=";
      s += std::to_wstring(sus.score);
      s += L"\r\n";
    }
  }

  if (!r.resources.empty()) {
    std::vector<std::wstring> items;
    const std::size_t n = std::min<std::size_t>(r.resources.size(), 6);
    items.reserve(n);
    for (std::size_t i = r.resources.size() - n; i < r.resources.size(); i++) {
      const auto& rr = r.resources[i];
      items.push_back(rr.path);
    }
    s += L"- ";
    s += LText(lang, L"Recent assets (.nif/.hkx/.tri)", L"최근 리소스(.nif/.hkx/.tri)");
    s += L": ";
    s += join(items, 6, L", ");
    s += L"\r\n";
  }

  wchar_t buf[128]{};
  swprintf_s(buf, L"- ExceptionCode: 0x%08X\r\n", r.exc_code);
  s += buf;

  swprintf_s(buf, L"- ExceptionAddress: 0x%llX\r\n", static_cast<unsigned long long>(r.exc_addr));
  s += buf;

  swprintf_s(buf, L"- ThreadId: %u\r\n", r.exc_tid);
  s += buf;

  swprintf_s(buf, L"- StateFlags: %u\r\n", r.state_flags);
  s += buf;

  s += L"\r\n";
  s += LText(lang, L"Recommended actions (summary)\r\n", L"권장 조치(요약)\r\n");
  if (r.recommendations.empty()) {
    s += L"- ";
    s += LText(lang, L"(none)", L"(없음)");
    s += L"\r\n";
  } else {
    const std::size_t n = std::min<std::size_t>(r.recommendations.size(), 6);
    for (std::size_t i = 0; i < n; i++) {
      s += L"- ";
      s += r.recommendations[i];
      s += L"\r\n";
    }
    if (r.recommendations.size() > n) {
      s += L"- ";
      s += LText(lang, L"(See the Evidence tab for the full checklist)", L"(나머지는 근거 탭의 체크리스트에서 확인)");
      s += L"\r\n";
    }
  }

  return s;
}

void SetTabVisible(AppState* st, int tabIndex)
{
  if (!st) {
    return;
  }
  ShowWindow(st->summaryEdit, (tabIndex == 0) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->evidenceList, (tabIndex == 1) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->evidenceSplitter, (tabIndex == 1) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->recoList, (tabIndex == 1) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->eventsFilterEdit, (tabIndex == 2) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->eventsList, (tabIndex == 2) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->resourcesList, (tabIndex == 3) ? SW_SHOW : SW_HIDE);
  ShowWindow(st->wctEdit, (tabIndex == 4) ? SW_SHOW : SW_HIDE);
}

void Layout(AppState* st)
{
  if (!st || !st->hwnd) {
    return;
  }

  const int padding = ScalePx(st->hwnd, 12);
  const int btnH = ScalePx(st->hwnd, 30);
  const int btnW = ScalePx(st->hwnd, 130);
  const int btnGap = ScalePx(st->hwnd, 8);

  RECT rc{};
  GetClientRect(st->hwnd, &rc);

  const int bottom = rc.bottom - padding;

  const int tabTop = padding;
  const int tabLeft = padding;
  const int tabRight = rc.right - padding;
  const int tabBottom = bottom - btnH - padding;

  MoveWindow(st->tab, tabLeft, tabTop, tabRight - tabLeft, tabBottom - tabTop, TRUE);

  RECT tabRc{};
  GetClientRect(st->tab, &tabRc);
  TabCtrl_AdjustRect(st->tab, FALSE, &tabRc);

  const int baseX = tabLeft + static_cast<int>(tabRc.left);
  const int baseY = tabTop + static_cast<int>(tabRc.top);

  const int w = static_cast<int>(tabRc.right - tabRc.left);
  const int h = static_cast<int>(tabRc.bottom - tabRc.top);

  MoveWindow(st->summaryEdit, baseX, baseY, w, h, TRUE);

  const int splitterH = ScalePx(st->hwnd, 8);
  const int split = baseY + (h * st->evidenceSplitPercent) / 100;
  const int splitTop = ClampInt(split - (splitterH / 2), baseY, baseY + h);
  const int splitBottom = ClampInt(splitTop + splitterH, baseY, baseY + h);

  MoveWindow(st->evidenceList, baseX, baseY, w, splitTop - baseY, TRUE);
  MoveWindow(st->evidenceSplitter, baseX, splitTop, w, splitBottom - splitTop, TRUE);
  MoveWindow(st->recoList, baseX, splitBottom, w, (baseY + h) - splitBottom, TRUE);

  const int filterH = ScalePx(st->hwnd, 28);
  const int filterGap = ScalePx(st->hwnd, 8);
  MoveWindow(st->eventsFilterEdit, baseX, baseY, w, filterH, TRUE);
  MoveWindow(st->eventsList, baseX, baseY + filterH + filterGap, w, h - filterH - filterGap, TRUE);

  MoveWindow(st->resourcesList, baseX, baseY, w, h, TRUE);
  MoveWindow(st->wctEdit, baseX, baseY, w, h, TRUE);

  const int y = bottom - btnH;
  int x = padding;
  MoveWindow(GetDlgItem(st->hwnd, kIdBtnOpenDump), x, y, btnW, btnH, TRUE);
  x += btnW + btnGap;
  MoveWindow(GetDlgItem(st->hwnd, kIdBtnOpenFolder), x, y, btnW, btnH, TRUE);
  x += btnW + btnGap;
  MoveWindow(GetDlgItem(st->hwnd, kIdBtnCopy), x, y, btnW, btnH, TRUE);
  x += btnW + btnGap;
  MoveWindow(GetDlgItem(st->hwnd, kIdBtnCopyChecklist), x, y, btnW, btnH, TRUE);

  const int langBtnW = ScalePx(st->hwnd, 90);
  const int langX = rc.right - padding - langBtnW;
  MoveWindow(GetDlgItem(st->hwnd, kIdBtnLanguage), langX, y, langBtnW, btnH, TRUE);
}

void ListViewAddColumn(HWND lv, int col, int width, const wchar_t* title)
{
  LVCOLUMNW c{};
  c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  c.pszText = const_cast<wchar_t*>(title);
  c.cx = width;
  c.iSubItem = col;
  ListView_InsertColumn(lv, col, &c);
}

void SetListViewColumnTitle(HWND lv, int col, const wchar_t* title)
{
  if (!lv) {
    return;
  }
  LVCOLUMNW c{};
  c.mask = LVCF_TEXT;
  c.pszText = const_cast<wchar_t*>(title);
  ListView_SetColumn(lv, col, &c);
}

void ApplyUiLanguage(AppState* st)
{
  if (!st) {
    return;
  }
  const auto lang = st->analyzeOpt.language;

  // Tabs
  if (st->tab) {
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<wchar_t*>(LText(lang, L"Summary", L"요약"));
    TabCtrl_SetItem(st->tab, 0, &ti);
    ti.pszText = const_cast<wchar_t*>(LText(lang, L"Evidence", L"근거"));
    TabCtrl_SetItem(st->tab, 1, &ti);
    ti.pszText = const_cast<wchar_t*>(LText(lang, L"Events", L"이벤트"));
    TabCtrl_SetItem(st->tab, 2, &ti);
    ti.pszText = const_cast<wchar_t*>(LText(lang, L"Resources", L"리소스"));
    TabCtrl_SetItem(st->tab, 3, &ti);
    ti.pszText = const_cast<wchar_t*>(L"WCT");
    TabCtrl_SetItem(st->tab, 4, &ti);
  }

  // Evidence list columns
  SetListViewColumnTitle(st->evidenceList, 0, LText(lang, L"Confidence", L"신뢰도"));
  SetListViewColumnTitle(st->evidenceList, 1, LText(lang, L"Finding", L"판단"));
  SetListViewColumnTitle(st->evidenceList, 2, LText(lang, L"Evidence", L"근거"));

  // Checklist column
  SetListViewColumnTitle(st->recoList, 0, LText(lang, L"Recommended actions (checklist)", L"권장 조치(체크리스트)"));

  // Event filter cue banner
  if (st->eventsFilterEdit) {
    SendMessageW(
      st->eventsFilterEdit,
      EM_SETCUEBANNER,
      TRUE,
      reinterpret_cast<LPARAM>(LText(lang, L"Filter events (type/tid/a/b/c/d)", L"이벤트 검색 (type/tid/a/b/c/d)")));
  }

  // Bottom buttons
  SetWindowTextW(GetDlgItem(st->hwnd, kIdBtnOpenDump), LText(lang, L"Open dump", L"덤프 열기"));
  SetWindowTextW(GetDlgItem(st->hwnd, kIdBtnOpenFolder), LText(lang, L"Open folder", L"폴더 열기"));
  SetWindowTextW(GetDlgItem(st->hwnd, kIdBtnCopy), LText(lang, L"Copy summary", L"요약 복사"));
  SetWindowTextW(GetDlgItem(st->hwnd, kIdBtnCopyChecklist), LText(lang, L"Copy checklist", L"체크리스트 복사"));

  std::wstring langBtn = L"Lang: ";
  langBtn += (lang == i18n::Language::kEnglish) ? L"EN" : L"KO";
  SetWindowTextW(GetDlgItem(st->hwnd, kIdBtnLanguage), langBtn.c_str());
}

std::wstring ToW(double v);
std::wstring ToW64(std::uint64_t v);

void PopulateEvidence(HWND lv, const AnalysisResult& r)
{
  ListView_DeleteAllItems(lv);
  for (int i = 0; i < static_cast<int>(r.evidence.size()); i++) {
    const auto& e = r.evidence[static_cast<std::size_t>(i)];
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = i;
    it.pszText = const_cast<wchar_t*>(e.confidence.c_str());
    it.lParam = static_cast<LPARAM>(e.confidence_level);
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, i, 1, const_cast<wchar_t*>(e.title.c_str()));
    ListView_SetItemText(lv, i, 2, const_cast<wchar_t*>(e.details.c_str()));
  }
}

bool ContainsI(std::wstring_view haystack, std::wstring_view needle)
{
  if (needle.empty()) {
    return true;
  }
  auto it = std::search(
    haystack.begin(),
    haystack.end(),
    needle.begin(),
    needle.end(),
    [](wchar_t a, wchar_t b) { return static_cast<wchar_t>(towlower(a)) == static_cast<wchar_t>(towlower(b)); });
  return it != haystack.end();
}

void PopulateEventsFiltered(HWND lv, const AnalysisResult& r, std::wstring_view filter)
{
  ListView_DeleteAllItems(lv);

  for (int i = 0; i < static_cast<int>(r.events.size()); i++) {
    const auto& e = r.events[static_cast<std::size_t>(i)];

    const std::wstring tms = ToW(e.t_ms);
    const std::wstring tid = ToW64(e.tid);
    const std::wstring a = ToW64(e.a);
    const std::wstring b = ToW64(e.b);
    const std::wstring c = ToW64(e.c);
    const std::wstring d = ToW64(e.d);

    if (!filter.empty()) {
      if (!ContainsI(e.type_name, filter) &&
          !ContainsI(tms, filter) &&
          !ContainsI(tid, filter) &&
          !ContainsI(a, filter) &&
          !ContainsI(b, filter) &&
          !ContainsI(c, filter) &&
          !ContainsI(d, filter)) {
        continue;
      }
    }

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = const_cast<wchar_t*>(tms.c_str());
    ListView_InsertItem(lv, &it);

    ListView_SetItemText(lv, it.iItem, 1, const_cast<wchar_t*>(e.type_name.c_str()));
    ListView_SetItemText(lv, it.iItem, 2, const_cast<wchar_t*>(tid.c_str()));
    ListView_SetItemText(lv, it.iItem, 3, const_cast<wchar_t*>(a.c_str()));
    ListView_SetItemText(lv, it.iItem, 4, const_cast<wchar_t*>(b.c_str()));
    ListView_SetItemText(lv, it.iItem, 5, const_cast<wchar_t*>(c.c_str()));
    ListView_SetItemText(lv, it.iItem, 6, const_cast<wchar_t*>(d.c_str()));
  }
}

void PopulateRecommendations(HWND lv, const AnalysisResult& r)
{
  ListView_DeleteAllItems(lv);
  for (int i = 0; i < static_cast<int>(r.recommendations.size()); i++) {
    const auto& s = r.recommendations[static_cast<std::size_t>(i)];
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = const_cast<wchar_t*>(s.c_str());
    ListView_InsertItem(lv, &it);
    ListView_SetCheckState(lv, i, TRUE);
  }
}

std::wstring ToW(double v)
{
  wchar_t buf[64]{};
  swprintf_s(buf, L"%.1f", v);
  return buf;
}

std::wstring ToW64(std::uint64_t v)
{
  wchar_t buf[64]{};
  swprintf_s(buf, L"%llu", static_cast<unsigned long long>(v));
  return buf;
}

std::wstring JoinListW(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep)
{
  if (items.empty() || maxN == 0) {
    return {};
  }
  const std::size_t n = std::min<std::size_t>(items.size(), maxN);
  std::wstring out;
  for (std::size_t i = 0; i < n; i++) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  if (items.size() > n) {
    out += sep;
    out += L"...";
  }
  return out;
}

void PopulateResources(HWND lv, const AnalysisResult& r)
{
  ListView_DeleteAllItems(lv);

  for (int i = 0; i < static_cast<int>(r.resources.size()); i++) {
    const auto& rr = r.resources[static_cast<std::size_t>(i)];

    const std::wstring tms = ToW(rr.t_ms);
    const std::wstring kind = rr.kind.empty() ? L"(unknown)" : rr.kind;
    const std::wstring tid = ToW64(rr.tid);
    const std::wstring path = rr.path;
    const std::wstring providers = rr.providers.empty() ? L"" : JoinListW(rr.providers, 6, L", ");

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = const_cast<wchar_t*>(tms.c_str());
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, i, 1, const_cast<wchar_t*>(kind.c_str()));
    ListView_SetItemText(lv, i, 2, const_cast<wchar_t*>(tid.c_str()));
    ListView_SetItemText(lv, i, 3, const_cast<wchar_t*>(path.c_str()));
    ListView_SetItemText(lv, i, 4, const_cast<wchar_t*>(providers.c_str()));
  }
}

void PopulateEvents(HWND lv, const AnalysisResult& r)
{
  ListView_DeleteAllItems(lv);
  for (int i = 0; i < static_cast<int>(r.events.size()); i++) {
    const auto& e = r.events[static_cast<std::size_t>(i)];

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = i;

    const std::wstring t = ToW(e.t_ms);
    it.pszText = const_cast<wchar_t*>(t.c_str());
    ListView_InsertItem(lv, &it);

    ListView_SetItemText(lv, i, 1, const_cast<wchar_t*>(e.type_name.c_str()));

    wchar_t buf[32]{};
    swprintf_s(buf, L"%u", e.tid);
    ListView_SetItemText(lv, i, 2, buf);

    const std::wstring a = ToW64(e.a);
    const std::wstring b = ToW64(e.b);
    const std::wstring c = ToW64(e.c);
    const std::wstring d = ToW64(e.d);
    ListView_SetItemText(lv, i, 3, const_cast<wchar_t*>(a.c_str()));
    ListView_SetItemText(lv, i, 4, const_cast<wchar_t*>(b.c_str()));
    ListView_SetItemText(lv, i, 5, const_cast<wchar_t*>(c.c_str()));
    ListView_SetItemText(lv, i, 6, const_cast<wchar_t*>(d.c_str()));
  }
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text)
{
  if (!OpenClipboard(hwnd)) {
    return false;
  }
  EmptyClipboard();

  const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!h) {
    CloseClipboard();
    return false;
  }

  void* p = GlobalLock(h);
  std::memcpy(p, text.c_str(), bytes);
  GlobalUnlock(h);

  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
  return true;
}

std::wstring CollectCheckedRecommendations(HWND lv)
{
  std::wstring out;
  const int n = ListView_GetItemCount(lv);
  for (int i = 0; i < n; i++) {
    if (ListView_GetCheckState(lv, i) == FALSE) {
      continue;
    }
    wchar_t buf[4096]{};
    ListView_GetItemText(lv, i, 0, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (buf[0] == 0) {
      continue;
    }
    out += L"- ";
    out += buf;
    out += L"\r\n";
  }
  return out;
}

std::optional<std::wstring> PickDumpFile(HWND owner)
{
  wchar_t fileBuf[MAX_PATH]{};

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Minidump (*.dmp)\0*.dmp\0All files\0*.*\0\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  ofn.lpstrDefExt = L"dmp";

  if (!GetOpenFileNameW(&ofn)) {
    return std::nullopt;
  }
  return std::wstring(fileBuf);
}

void OpenFolder(const std::wstring& path)
{
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RefreshUi(AppState* st)
{
  if (!st) {
    return;
  }

  ApplyUiLanguage(st);

  const std::wstring summary = FormatSummaryText(st->r);
  SetWindowTextW(st->summaryEdit, summary.c_str());
  PopulateEvidence(st->evidenceList, st->r);
  PopulateRecommendations(st->recoList, st->r);
  wchar_t filterBuf[512]{};
  GetWindowTextW(st->eventsFilterEdit, filterBuf, static_cast<int>(sizeof(filterBuf) / sizeof(filterBuf[0])));
  PopulateEventsFiltered(st->eventsList, st->r, filterBuf);

  PopulateResources(st->resourcesList, st->r);

  const std::wstring wctText = FormatWctText(st->r);
  SetWindowTextW(st->wctEdit, wctText.c_str());

  std::wstring title = LText(st->analyzeOpt.language, L"Tullius CTD Logger - Viewer", L"툴리우스 CTD 로거 - 뷰어");
  if (!st->r.dump_path.empty()) {
    title += L" - ";
    title += std::filesystem::path(st->r.dump_path).filename().wstring();
  }
  SetWindowTextW(st->hwnd, title.c_str());
}

bool AnalyzeAndUpdate(AppState* st, const std::wstring& dumpPath)
{
  if (!st) {
    return false;
  }

  const auto lang = st->analyzeOpt.language;
  const std::filesystem::path dp(dumpPath);
  const std::filesystem::path out = st->r.out_dir.empty() ? dp.parent_path() : std::filesystem::path(st->r.out_dir);

  AnalysisResult r{};
  std::wstring err;
  if (!AnalyzeDump(dumpPath, out.wstring(), st->analyzeOpt, r, &err)) {
    st->lastError = err;
    MessageBoxW(
      st->hwnd,
      (std::wstring(LText(lang, L"Dump analysis failed:\n", L"덤프 분석 실패:\n")) + err).c_str(),
      L"SkyrimDiagDumpTool",
      MB_ICONERROR);
    return false;
  }

  if (!WriteOutputs(r, &err)) {
    st->lastError = err;
    MessageBoxW(
      st->hwnd,
      (std::wstring(LText(lang, L"Failed to write output files:\n", L"결과 파일 생성 실패:\n")) + err).c_str(),
      L"SkyrimDiagDumpTool",
      MB_ICONERROR);
    return false;
  }

  st->r = std::move(r);
  RefreshUi(st);
  return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_ERASEBKGND: {
      if (!st || !st->bgBaseBrush) {
        break;
      }
      HDC hdc = reinterpret_cast<HDC>(wParam);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, st->bgBaseBrush);
      return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      if (!st) {
        break;
      }
      const HWND ctl = reinterpret_cast<HWND>(lParam);
      const HDC hdc = reinterpret_cast<HDC>(wParam);
      if ((ctl == st->summaryEdit || ctl == st->wctEdit || ctl == st->eventsFilterEdit) && st->bgCardBrush) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, theme::TEXT_PRIMARY);
        SetBkColor(hdc, theme::BG_INPUT);
        return reinterpret_cast<INT_PTR>(st->bgCardBrush);
      }
      break;
    }
    
    case WM_CTLCOLORLISTBOX: {
      if (!st) break;
      const HDC hdc = reinterpret_cast<HDC>(wParam);
      SetTextColor(hdc, theme::TEXT_PRIMARY);
      SetBkColor(hdc, theme::BG_CARD);
      return reinterpret_cast<INT_PTR>(st->bgCardBrush);
    }
    
    case WM_CTLCOLORBTN: {
      if (!st) break;
      const HDC hdc = reinterpret_cast<HDC>(wParam);
      SetTextColor(hdc, theme::TEXT_PRIMARY);
      SetBkColor(hdc, theme::BG_BASE);
      return reinterpret_cast<INT_PTR>(st->bgBaseBrush);
    }

    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      st = reinterpret_cast<AppState*>(cs->lpCreateParams);
      st->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      st->uiFont = CreateUiFont(hwnd);
      st->uiFontSemibold = CreateUiFontSemibold(hwnd);
      st->uiFontMono = CreateUiFontMono(hwnd);
      // Skyrim Dark Fantasy Theme brushes
      st->bgBaseBrush = CreateSolidBrush(theme::BG_BASE);
      st->bgCardBrush = CreateSolidBrush(theme::BG_INPUT);
      
      // Enable dark mode for window
      EnableDarkMode(hwnd);

      st->tab = CreateWindowExW(
        0,
        WC_TABCONTROLW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdTab),
        cs->hInstance,
        nullptr);

      TabCtrl_SetPadding(st->tab, ScalePx(hwnd, 12), ScalePx(hwnd, 6));

      const auto lang = st->analyzeOpt.language;

      TCITEMW ti{};
      ti.mask = TCIF_TEXT;
      ti.pszText = const_cast<wchar_t*>(LText(lang, L"Summary", L"요약"));
      TabCtrl_InsertItem(st->tab, 0, &ti);
      ti.pszText = const_cast<wchar_t*>(LText(lang, L"Evidence", L"근거"));
      TabCtrl_InsertItem(st->tab, 1, &ti);
      ti.pszText = const_cast<wchar_t*>(LText(lang, L"Events", L"이벤트"));
      TabCtrl_InsertItem(st->tab, 2, &ti);
      ti.pszText = const_cast<wchar_t*>(LText(lang, L"Resources", L"리소스"));
      TabCtrl_InsertItem(st->tab, 3, &ti);
      ti.pszText = const_cast<wchar_t*>(L"WCT");
      TabCtrl_InsertItem(st->tab, 4, &ti);

      st->summaryEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdSummaryEdit),
        cs->hInstance,
        nullptr);
      ApplyDarkEdit(st->summaryEdit);
      ApplyEditPadding(st->summaryEdit, ScalePx(hwnd, 10));

      st->evidenceList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdEvidenceList),
        cs->hInstance,
        nullptr);
      ApplyListViewModernStyle(st->evidenceList);
      ListViewAddColumn(st->evidenceList, 0, 70, LText(lang, L"Confidence", L"신뢰도"));
      ListViewAddColumn(st->evidenceList, 1, 240, LText(lang, L"Finding", L"판단"));
      ListViewAddColumn(st->evidenceList, 2, 800, LText(lang, L"Evidence", L"근거"));

      st->evidenceSplitter = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        100,
        6,
        hwnd,
        reinterpret_cast<HMENU>(kIdEvidenceSplitter),
        cs->hInstance,
        nullptr);
      SetWindowSubclass(
        st->evidenceSplitter,
        EvidenceSplitterSubclassProc,
        1,
        reinterpret_cast<DWORD_PTR>(st));

      st->recoList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdRecoList),
        cs->hInstance,
        nullptr);
      ListView_SetExtendedListViewStyle(
        st->recoList,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
      ApplyListViewModernStyle(st->recoList);
      ListViewAddColumn(st->recoList, 0, 1000, LText(lang, L"Recommended actions (checklist)", L"권장 조치(체크리스트)"));

      st->eventsFilterEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0,
        0,
        100,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdEventsFilterEdit),
        cs->hInstance,
        nullptr);
      ApplyDarkEdit(st->eventsFilterEdit);
      ApplyEditPadding(st->eventsFilterEdit, ScalePx(hwnd, 10));
      SendMessageW(
        st->eventsFilterEdit,
        EM_SETCUEBANNER,
        TRUE,
        reinterpret_cast<LPARAM>(LText(lang, L"Filter events (type/tid/a/b/c/d)", L"이벤트 검색 (type/tid/a/b/c/d)")));

      st->eventsList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdEventsList),
        cs->hInstance,
        nullptr);
      ApplyListViewModernStyle(st->eventsList);
      ListViewAddColumn(st->eventsList, 0, 80, L"t_ms");
      ListViewAddColumn(st->eventsList, 1, 120, L"type");
      ListViewAddColumn(st->eventsList, 2, 70, L"tid");
      ListViewAddColumn(st->eventsList, 3, 120, L"a");
      ListViewAddColumn(st->eventsList, 4, 120, L"b");
      ListViewAddColumn(st->eventsList, 5, 120, L"c");
      ListViewAddColumn(st->eventsList, 6, 120, L"d");

      st->resourcesList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdResourcesList),
        cs->hInstance,
        nullptr);
      ApplyListViewModernStyle(st->resourcesList);
      ListViewAddColumn(st->resourcesList, 0, 80, L"t_ms");
      ListViewAddColumn(st->resourcesList, 1, 60, L"type");
      ListViewAddColumn(st->resourcesList, 2, 70, L"tid");
      ListViewAddColumn(st->resourcesList, 3, 520, L"path");
      ListViewAddColumn(st->resourcesList, 4, 360, L"providers");

      st->wctEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
        0,
        0,
        100,
        100,
        hwnd,
        reinterpret_cast<HMENU>(kIdWctEdit),
        cs->hInstance,
        nullptr);
      ApplyDarkEdit(st->wctEdit);
      ApplyEditPadding(st->wctEdit, ScalePx(hwnd, 10));

      CreateWindowExW(
        0,
        L"BUTTON",
        LText(lang, L"Open dump", L"덤프 열기"),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0,
        0,
        100,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnOpenDump),
        cs->hInstance,
        nullptr);

      CreateWindowExW(
        0,
        L"BUTTON",
        LText(lang, L"Open folder", L"폴더 열기"),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0,
        0,
        100,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnOpenFolder),
        cs->hInstance,
        nullptr);

      CreateWindowExW(
        0,
        L"BUTTON",
        LText(lang, L"Copy summary", L"요약 복사"),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0,
        0,
        100,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnCopy),
        cs->hInstance,
        nullptr);

      CreateWindowExW(
        0,
        L"BUTTON",
        LText(lang, L"Copy checklist", L"체크리스트 복사"),
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0,
        0,
        120,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnCopyChecklist),
        cs->hInstance,
        nullptr);

      CreateWindowExW(
        0,
        L"BUTTON",
        L"Lang: EN",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0,
        0,
        90,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnLanguage),
        cs->hInstance,
        nullptr);

      ApplyDarkTab(st->tab);

      ApplyFont(st->tab, st->uiFont);
      ApplyFont(st->summaryEdit, st->uiFont);
      ApplyFont(st->evidenceList, st->uiFont);
      ApplyFont(st->recoList, st->uiFont);
      ApplyFont(st->eventsFilterEdit, st->uiFont);
      ApplyFont(st->eventsList, st->uiFont);
      ApplyFont(st->resourcesList, st->uiFont);
      ApplyFont(st->wctEdit, st->uiFontMono ? st->uiFontMono : st->uiFont);

      for (int bid : { kIdBtnOpenDump, kIdBtnOpenFolder, kIdBtnCopy, kIdBtnCopyChecklist, kIdBtnLanguage }) {
        HWND btn = GetDlgItem(hwnd, bid);
        ApplyFont(btn, st->uiFont);
        ApplyDarkButton(btn);
      }

      // Increase row height using an imagelist trick.
      const int rowH = ScalePx(hwnd, 28);
      st->rowHeightImages = ImageList_Create(1, rowH, ILC_COLOR32, 1, 0);
      if (st->rowHeightImages) {
        ListView_SetImageList(st->evidenceList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->recoList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->eventsList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->resourcesList, st->rowHeightImages, LVSIL_SMALL);
      }

      DragAcceptFiles(hwnd, TRUE);
      Layout(st);
      SetTabVisible(st, 0);
      RefreshUi(st);
      return 0;
    }

    case WM_DRAWITEM: {
      if (!st) {
        break;
      }
      const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
      if (!dis) {
        break;
      }
      if (dis->CtlType != ODT_BUTTON) {
        break;
      }

      const int id = static_cast<int>(dis->CtlID);
      const bool isPrimary = (id == kIdBtnOpenDump);
      DrawModernButton(st, dis, isPrimary);
      return TRUE;
    }

    case WM_SIZE: {
      if (st) {
        Layout(st);
      }
      return 0;
    }

    case WM_DPICHANGED: {
      if (!st) {
        break;
      }

      auto* prc = reinterpret_cast<RECT*>(lParam);
      SetWindowPos(
        hwnd,
        nullptr,
        prc->left,
        prc->top,
        prc->right - prc->left,
        prc->bottom - prc->top,
        SWP_NOZORDER | SWP_NOACTIVATE);

      if (st->uiFont) {
        DeleteObject(st->uiFont);
      }
      st->uiFont = CreateUiFont(hwnd);

      if (st->uiFontSemibold) {
        DeleteObject(st->uiFontSemibold);
      }
      st->uiFontSemibold = CreateUiFontSemibold(hwnd);

      if (st->uiFontMono) {
        DeleteObject(st->uiFontMono);
      }
      st->uiFontMono = CreateUiFontMono(hwnd);

      TabCtrl_SetPadding(st->tab, ScalePx(hwnd, 12), ScalePx(hwnd, 6));

      ApplyFont(st->tab, st->uiFont);
      ApplyFont(st->summaryEdit, st->uiFont);
      ApplyFont(st->evidenceList, st->uiFont);
      ApplyFont(st->recoList, st->uiFont);
      ApplyFont(st->eventsFilterEdit, st->uiFont);
      ApplyFont(st->eventsList, st->uiFont);
      ApplyFont(st->resourcesList, st->uiFont);
      ApplyFont(st->wctEdit, st->uiFontMono ? st->uiFontMono : st->uiFont);
      ApplyEditPadding(st->summaryEdit, ScalePx(hwnd, 10));
      ApplyEditPadding(st->eventsFilterEdit, ScalePx(hwnd, 10));
      ApplyEditPadding(st->wctEdit, ScalePx(hwnd, 10));
      for (int bid : { kIdBtnOpenDump, kIdBtnOpenFolder, kIdBtnCopy, kIdBtnCopyChecklist, kIdBtnLanguage }) {
        ApplyFont(GetDlgItem(hwnd, bid), st->uiFont);
      }

      if (st->rowHeightImages) {
        ImageList_Destroy(st->rowHeightImages);
        st->rowHeightImages = nullptr;
      }
      const int rowH = ScalePx(hwnd, 28);
      st->rowHeightImages = ImageList_Create(1, rowH, ILC_COLOR32, 1, 0);
      if (st->rowHeightImages) {
        ListView_SetImageList(st->evidenceList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->recoList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->eventsList, st->rowHeightImages, LVSIL_SMALL);
        ListView_SetImageList(st->resourcesList, st->rowHeightImages, LVSIL_SMALL);
      }

      Layout(st);
      RefreshUi(st);
      return 0;
    }

    case WM_NOTIFY: {
      if (!st) {
        break;
      }

      auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
      if (hdr->hwndFrom == st->tab && hdr->code == TCN_SELCHANGE) {
        const int idx = TabCtrl_GetCurSel(st->tab);
        SetTabVisible(st, idx);
        return 0;
      }

      if (hdr->hwndFrom == st->evidenceList && hdr->code == NM_CUSTOMDRAW) {
        return HandleEvidenceCustomDraw(st, reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam));
      }
      break;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (!st) {
        break;
      }
      const auto lang = st->analyzeOpt.language;

      if (id == kIdEventsFilterEdit && HIWORD(wParam) == EN_CHANGE) {
        wchar_t filterBuf[512]{};
        GetWindowTextW(st->eventsFilterEdit, filterBuf, static_cast<int>(sizeof(filterBuf) / sizeof(filterBuf[0])));
        PopulateEventsFiltered(st->eventsList, st->r, filterBuf);
        return 0;
      }

      if (id == kIdBtnOpenDump) {
        auto picked = PickDumpFile(hwnd);
        if (picked) {
          AnalyzeAndUpdate(st, *picked);
        }
        return 0;
      }
      if (id == kIdBtnOpenFolder) {
        const std::filesystem::path p = st->r.out_dir.empty() ? std::filesystem::path(st->r.dump_path).parent_path()
                                                             : std::filesystem::path(st->r.out_dir);
        OpenFolder(p.wstring());
        return 0;
      }
      if (id == kIdBtnCopy) {
        const std::wstring summary = FormatSummaryText(st->r);
        CopyTextToClipboard(hwnd, summary);
        MessageBoxW(hwnd, LText(lang, L"Copied to clipboard.", L"클립보드로 복사했습니다."), L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
        return 0;
      }
      if (id == kIdBtnCopyChecklist) {
        const std::wstring s = CollectCheckedRecommendations(st->recoList);
        if (s.empty()) {
          MessageBoxW(hwnd, LText(lang, L"No items checked.", L"체크된 항목이 없습니다."), L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
          return 0;
        }
        CopyTextToClipboard(hwnd, s);
        MessageBoxW(
          hwnd,
          LText(lang, L"Copied checklist to clipboard.", L"체크리스트를 클립보드로 복사했습니다."),
          L"SkyrimDiagDumpTool",
          MB_ICONINFORMATION);
        return 0;
      }
      if (id == kIdBtnLanguage) {
        st->analyzeOpt.language = (st->analyzeOpt.language == i18n::Language::kEnglish)
          ? i18n::Language::kKorean
          : i18n::Language::kEnglish;

        DumpToolConfig cfg{};
        cfg.language = st->analyzeOpt.language;
        std::wstring cfgErr;
        if (!SaveDumpToolConfig(cfg, &cfgErr)) {
          MessageBoxW(
            hwnd,
            (std::wstring(LText(st->analyzeOpt.language, L"Failed to save config:\n", L"설정 저장 실패:\n")) + cfgErr).c_str(),
            L"SkyrimDiagDumpTool",
            MB_ICONWARNING);
        }

        ApplyUiLanguage(st);
        if (!st->r.dump_path.empty()) {
          AnalyzeAndUpdate(st, st->r.dump_path);
        } else {
          RefreshUi(st);
        }
        return 0;
      }
      break;
    }

    case WM_DROPFILES: {
      if (!st) {
        break;
      }
      HDROP h = reinterpret_cast<HDROP>(wParam);
      wchar_t buf[MAX_PATH]{};
      if (DragQueryFileW(h, 0, buf, MAX_PATH) > 0) {
        AnalyzeAndUpdate(st, buf);
      }
      DragFinish(h);
      return 0;
    }

    case WM_DESTROY:
      if (st && st->uiFont) {
        DeleteObject(st->uiFont);
        st->uiFont = nullptr;
      }
      if (st && st->uiFontSemibold) {
        DeleteObject(st->uiFontSemibold);
        st->uiFontSemibold = nullptr;
      }
      if (st && st->uiFontMono) {
        DeleteObject(st->uiFontMono);
        st->uiFontMono = nullptr;
      }
      if (st && st->bgBaseBrush) {
        DeleteObject(st->bgBaseBrush);
        st->bgBaseBrush = nullptr;
      }
      if (st && st->bgCardBrush) {
        DeleteObject(st->bgCardBrush);
        st->bgCardBrush = nullptr;
      }
      if (st && st->rowHeightImages) {
        ImageList_Destroy(st->rowHeightImages);
        st->rowHeightImages = nullptr;
      }
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int RunGuiViewer(HINSTANCE hInst, const GuiOptions& opt, const AnalyzeOptions& analyzeOpt, AnalysisResult initial, std::wstring* err)
{
  if (auto p = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(theme::BG_BASE);
  wc.lpszClassName = kWndClassName;
  RegisterClassExW(&wc);

  AppState st{};
  st.guiOpt = opt;
  st.analyzeOpt = analyzeOpt;
  st.r = std::move(initial);

  HWND hwnd = CreateWindowExW(
    0,
    kWndClassName,
    LText(analyzeOpt.language, L"Tullius CTD Logger - Viewer", L"툴리우스 CTD 로거 - 뷰어"),
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    1100,
    750,
    nullptr,
    nullptr,
    hInst,
    &st);
  if (!hwnd) {
    if (err) *err = L"CreateWindowExW failed";
    return 2;
  }

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (err) err->clear();
  return static_cast<int>(msg.wParam);
}

}  // namespace skydiag::dump_tool
