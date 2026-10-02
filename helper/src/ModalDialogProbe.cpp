#include "SkyrimDiagHelper/ModalDialogProbe.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "HelperCommon.h"

namespace skydiag::helper {
namespace {

using skydiag::helper::internal::WideToUtf8;

std::wstring ReadWindowTextBounded(HWND hwnd, std::size_t maxChars)
{
  // GetWindowTextW does not send WM_GETTEXT to windows of another process; it
  // returns the text the system already stores for the window.
  std::wstring buf(maxChars + 1u, L'\0');
  const int n = GetWindowTextW(hwnd, buf.data(), static_cast<int>(buf.size()));
  if (n <= 0) {
    return {};
  }
  buf.resize(static_cast<std::size_t>(n));
  return buf;
}

struct StaticTextCtx
{
  std::wstring text;
};

BOOL CALLBACK EnumDialogStaticText(HWND child, LPARAM lParam)
{
  auto* ctx = reinterpret_cast<StaticTextCtx*>(lParam);
  if (!ctx || ctx->text.size() >= kModalDialogMaxTextChars) {
    return FALSE;
  }

  wchar_t cls[32]{};
  if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0 || _wcsicmp(cls, L"Static") != 0) {
    return TRUE;
  }
  const auto style = static_cast<DWORD>(GetWindowLongW(child, GWL_STYLE));
  const DWORD type = style & SS_TYPEMASK;
  if (type == SS_ICON || type == SS_BITMAP || type == SS_ENHMETAFILE) {
    return TRUE;
  }

  const auto remaining = kModalDialogMaxTextChars - ctx->text.size();
  auto piece = ReadWindowTextBounded(child, remaining);
  if (piece.empty()) {
    return TRUE;
  }
  if (!ctx->text.empty()) {
    ctx->text += L'\n';
  }
  ctx->text += piece;
  if (ctx->text.size() > kModalDialogMaxTextChars) {
    ctx->text.resize(kModalDialogMaxTextChars);
  }
  return TRUE;
}

struct EnumDialogsCtx
{
  DWORD pid = 0;
  std::vector<ModalDialogWindow>* out = nullptr;
};

BOOL CALLBACK EnumTopLevelDialogs(HWND hwnd, LPARAM lParam)
{
  auto* ctx = reinterpret_cast<EnumDialogsCtx*>(lParam);
  if (!ctx || !ctx->out) {
    return FALSE;
  }
  if (ctx->out->size() >= kModalDialogMaxCount) {
    return FALSE;
  }

  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid || tid == 0 || !IsWindowVisible(hwnd)) {
    return TRUE;
  }

  wchar_t cls[32]{};
  if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0 || std::wcscmp(cls, L"#32770") != 0) {
    return TRUE;
  }

  ModalDialogWindow dialog{};
  dialog.tid = tid;
  dialog.title = WideToUtf8(ReadWindowTextBounded(hwnd, kModalDialogMaxTitleChars));

  StaticTextCtx textCtx{};
  EnumChildWindows(hwnd, EnumDialogStaticText, reinterpret_cast<LPARAM>(&textCtx));
  dialog.text = WideToUtf8(textCtx.text);

  if (const HWND owner = GetWindow(hwnd, GW_OWNER); owner != nullptr) {
    dialog.has_owner = true;
    dialog.owner_disabled = IsWindowEnabled(owner) == FALSE;
  }

  ctx->out->push_back(std::move(dialog));
  return TRUE;
}

}  // namespace

std::vector<ModalDialogWindow> CaptureModalDialogs(std::uint32_t pid)
{
  std::vector<ModalDialogWindow> dialogs;
  if (pid == 0u) {
    return dialogs;
  }
  EnumDialogsCtx ctx{};
  ctx.pid = static_cast<DWORD>(pid);
  ctx.out = &dialogs;
  EnumWindows(EnumTopLevelDialogs, reinterpret_cast<LPARAM>(&ctx));
  return dialogs;
}

nlohmann::json ModalDialogsToJson(const std::vector<ModalDialogWindow>& dialogs)
{
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& dialog : dialogs) {
    arr.push_back({
      { "tid", dialog.tid },
      { "class", "#32770" },
      { "title", dialog.title },
      { "text", dialog.text },
      { "has_owner", dialog.has_owner },
      { "owner_disabled", dialog.owner_disabled },
    });
  }
  return arr;
}

}  // namespace skydiag::helper
