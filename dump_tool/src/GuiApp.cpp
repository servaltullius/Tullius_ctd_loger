#include "GuiApp.h"
#include "Utf.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

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

void ApplyListViewModernStyle(HWND lv)
{
  if (!lv) {
    return;
  }

  // Remove gridlines for a more modern look.
  DWORD ex = ListView_GetExtendedListViewStyle(lv);
  ex &= ~static_cast<DWORD>(LVS_EX_GRIDLINES);
  ex |= (LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ListView_SetExtendedListViewStyle(lv, ex);

  // White "card" background.
  ListView_SetBkColor(lv, RGB(255, 255, 255));
  ListView_SetTextBkColor(lv, RGB(255, 255, 255));
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

      if (col == 0) {
        wchar_t text[32]{};
        ListView_GetItemText(cd->nmcd.hdr.hwndFrom, static_cast<int>(cd->nmcd.dwItemSpec), 0, text, 32);

        COLORREF fg = RGB(185, 28, 28);
        COLORREF bg = RGB(254, 226, 226);
        if (wcscmp(text, L"높음") == 0) {
          fg = RGB(21, 128, 61);
          bg = RGB(220, 252, 231);
        } else if (wcscmp(text, L"중간") == 0) {
          fg = RGB(161, 98, 7);
          bg = RGB(254, 249, 195);
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

        // Base cell background (card white).
        HBRUSH cellBg = CreateSolidBrush(RGB(255, 255, 255));
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
        HPEN badgePen = CreatePen(PS_SOLID, 1, fg);
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

      // Default text colors for other columns.
      cd->clrText = RGB(17, 24, 39);
      cd->clrTextBk = RGB(255, 255, 255);
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
  if (!r.has_wct || r.wct_json_utf8.empty()) {
    return L"(WCT 정보 없음)";
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
    out += L"WCT (Wait Chain) 보기\r\n";
    out += L"- threads=" + std::to_wstring(threads.size()) + L"\r\n";
    out += L"- isCycle=true threads=" + std::to_wstring(cycles) + L"\r\n";
    out += L"- isCycle=true면 데드락 가능성이 큽니다.\r\n";
    out += L"- 이 화면은 '프리징/무한로딩' 진단용입니다.\r\n";
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
        out += L"  (nodes 없음)\r\n\r\n";
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
        out += L"  ... (노드 " + std::to_wstring(t.nodes.size() - nodeN) + L"개 생략)\r\n";
      }
      out += L"\r\n";
    }
    if (threads.size() > showN) {
      out += L"... (스레드 " + std::to_wstring(threads.size() - showN) + L"개 생략)\r\n";
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

  std::wstring s;
  s += L"결론\n";
  s += L"- ";
  s += r.summary_sentence.empty() ? L"(없음)" : r.summary_sentence;
  s += L"\r\n\r\n";

  s += L"기본 정보\r\n";
  s += L"- Dump: ";
  s += r.dump_path;
  s += L"\r\n";

  s += L"- Module+Offset: ";
  s += r.fault_module_plus_offset.empty() ? L"(unknown)" : r.fault_module_plus_offset;
  s += L"\r\n";

  if (!r.inferred_mod_name.empty()) {
    s += L"- 추정 모드: ";
    s += r.inferred_mod_name;
    s += L"\r\n";
  }

  if (!r.crash_logger_log_path.empty()) {
    s += L"- Crash Logger 로그: ";
    s += r.crash_logger_log_path;
    s += L"\r\n";
  }

  if (!r.crash_logger_top_modules.empty()) {
    s += L"- Crash Logger 상위 모듈: ";
    s += join(r.crash_logger_top_modules, 6, L", ");
    s += L"\r\n";
  }

  if (!r.suspects.empty()) {
    s += L"- 스택 스캔 후보(Top 5):\r\n";
    const std::size_t n = std::min<std::size_t>(r.suspects.size(), 5);
    for (std::size_t i = 0; i < n; i++) {
      const auto& sus = r.suspects[i];
      s += L"  - [";
      s += sus.confidence.empty() ? L"중간" : sus.confidence;
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
    s += L"- 최근 리소스(.nif/.hkx/.tri): ";
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

  s += L"\r\n권장 조치(요약)\r\n";
  if (r.recommendations.empty()) {
    s += L"- (없음)\r\n";
  } else {
    const std::size_t n = std::min<std::size_t>(r.recommendations.size(), 6);
    for (std::size_t i = 0; i < n; i++) {
      s += L"- ";
      s += r.recommendations[i];
      s += L"\r\n";
    }
    if (r.recommendations.size() > n) {
      s += L"- (나머지는 근거 탭의 체크리스트에서 확인)\r\n";
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

std::wstring ToW(double v);
std::wstring ToW64(std::uint64_t v);

void PopulateEvidence(HWND lv, const AnalysisResult& r)
{
  ListView_DeleteAllItems(lv);
  for (int i = 0; i < static_cast<int>(r.evidence.size()); i++) {
    const auto& e = r.evidence[static_cast<std::size_t>(i)];
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = const_cast<wchar_t*>(e.confidence.c_str());
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

  std::wstring title = L"SkyrimDiag Viewer";
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

  const std::filesystem::path dp(dumpPath);
  const std::filesystem::path out = st->r.out_dir.empty() ? dp.parent_path() : std::filesystem::path(st->r.out_dir);

  AnalysisResult r{};
  std::wstring err;
  if (!AnalyzeDump(dumpPath, out.wstring(), st->analyzeOpt, r, &err)) {
    st->lastError = err;
    MessageBoxW(st->hwnd, (L"덤프 분석 실패:\n" + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
    return false;
  }

  if (!WriteOutputs(r, &err)) {
    st->lastError = err;
    MessageBoxW(st->hwnd, (L"결과 파일 생성 실패:\n" + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
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
        SetTextColor(hdc, RGB(17, 24, 39));
        SetBkColor(hdc, RGB(255, 255, 255));
        return reinterpret_cast<INT_PTR>(st->bgCardBrush);
      }
      break;
    }

    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      st = reinterpret_cast<AppState*>(cs->lpCreateParams);
      st->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      st->uiFont = CreateUiFont(hwnd);
      st->uiFontSemibold = CreateUiFontSemibold(hwnd);
      st->bgBaseBrush = CreateSolidBrush(RGB(249, 250, 251));
      st->bgCardBrush = CreateSolidBrush(RGB(255, 255, 255));

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

      TCITEMW ti{};
      ti.mask = TCIF_TEXT;
      ti.pszText = const_cast<wchar_t*>(L"요약");
      TabCtrl_InsertItem(st->tab, 0, &ti);
      ti.pszText = const_cast<wchar_t*>(L"근거");
      TabCtrl_InsertItem(st->tab, 1, &ti);
      ti.pszText = const_cast<wchar_t*>(L"이벤트");
      TabCtrl_InsertItem(st->tab, 2, &ti);
      ti.pszText = const_cast<wchar_t*>(L"리소스");
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
      ListViewAddColumn(st->evidenceList, 0, 70, L"신뢰도");
      ListViewAddColumn(st->evidenceList, 1, 240, L"판단");
      ListViewAddColumn(st->evidenceList, 2, 800, L"근거");

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
      ListViewAddColumn(st->recoList, 0, 1000, L"권장 조치(체크리스트)");

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
      SendMessageW(
        st->eventsFilterEdit,
        EM_SETCUEBANNER,
        TRUE,
        reinterpret_cast<LPARAM>(L"이벤트 검색 (type/tid/a/b/c/d)"));

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

      CreateWindowExW(
        0,
        L"BUTTON",
        L"덤프 열기",
        WS_CHILD | WS_VISIBLE,
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
        L"폴더 열기",
        WS_CHILD | WS_VISIBLE,
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
        L"요약 복사",
        WS_CHILD | WS_VISIBLE,
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
        L"체크리스트 복사",
        WS_CHILD | WS_VISIBLE,
        0,
        0,
        120,
        28,
        hwnd,
        reinterpret_cast<HMENU>(kIdBtnCopyChecklist),
        cs->hInstance,
        nullptr);

      ApplyExplorerTheme(st->tab);
      ApplyExplorerTheme(st->evidenceList);
      ApplyExplorerTheme(st->recoList);
      ApplyExplorerTheme(st->eventsList);
      ApplyExplorerTheme(st->resourcesList);

      ApplyFont(st->tab, st->uiFont);
      ApplyFont(st->summaryEdit, st->uiFont);
      ApplyFont(st->evidenceList, st->uiFont);
      ApplyFont(st->recoList, st->uiFont);
      ApplyFont(st->eventsFilterEdit, st->uiFont);
      ApplyFont(st->eventsList, st->uiFont);
      ApplyFont(st->resourcesList, st->uiFont);
      ApplyFont(st->wctEdit, st->uiFont);

      for (int bid : { kIdBtnOpenDump, kIdBtnOpenFolder, kIdBtnCopy, kIdBtnCopyChecklist }) {
        ApplyFont(GetDlgItem(hwnd, bid), st->uiFont);
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

      TabCtrl_SetPadding(st->tab, ScalePx(hwnd, 12), ScalePx(hwnd, 6));

      ApplyFont(st->tab, st->uiFont);
      ApplyFont(st->summaryEdit, st->uiFont);
      ApplyFont(st->evidenceList, st->uiFont);
      ApplyFont(st->recoList, st->uiFont);
      ApplyFont(st->eventsFilterEdit, st->uiFont);
      ApplyFont(st->eventsList, st->uiFont);
      ApplyFont(st->resourcesList, st->uiFont);
      ApplyFont(st->wctEdit, st->uiFont);
      for (int bid : { kIdBtnOpenDump, kIdBtnOpenFolder, kIdBtnCopy, kIdBtnCopyChecklist }) {
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
        MessageBoxW(hwnd, L"클립보드로 복사했습니다.", L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
        return 0;
      }
      if (id == kIdBtnCopyChecklist) {
        const std::wstring s = CollectCheckedRecommendations(st->recoList);
        if (s.empty()) {
          MessageBoxW(hwnd, L"체크된 항목이 없습니다.", L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
          return 0;
        }
        CopyTextToClipboard(hwnd, s);
        MessageBoxW(hwnd, L"체크리스트를 클립보드로 복사했습니다.", L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
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
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWndClassName;
  RegisterClassExW(&wc);

  AppState st{};
  st.guiOpt = opt;
  st.analyzeOpt = analyzeOpt;
  st.r = std::move(initial);

  HWND hwnd = CreateWindowExW(
    0,
    kWndClassName,
    L"SkyrimDiag Viewer",
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
