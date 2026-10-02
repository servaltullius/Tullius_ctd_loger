#include "ModalDialogWait.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "Utf.h"

namespace skydiag::dump_tool {
namespace {

std::wstring LowerAscii(std::wstring_view value)
{
  std::wstring out(value);
  for (auto& ch : out) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
  }
  return out;
}

constexpr std::array<std::wstring_view, 19> kUser32ModalApis = {
  // Exported entry points (resolvable from export tables without PDBs).
  L"MessageBoxA",
  L"MessageBoxW",
  L"MessageBoxExA",
  L"MessageBoxExW",
  L"MessageBoxIndirectA",
  L"MessageBoxIndirectW",
  L"MessageBoxTimeoutA",
  L"MessageBoxTimeoutW",
  L"SoftModalMessageBox",
  L"DialogBoxParamA",
  L"DialogBoxParamW",
  L"DialogBoxIndirectParamA",
  L"DialogBoxIndirectParamW",
  L"DialogBoxIndirectParamAorW",
  // Internal names that appear when public user32 symbols are available.
  L"MessageBoxWorker",
  L"InternalDialogBox",
  L"DialogBox2",
  L"SoftModalMessageBoxWorker",
  L"MessageBoxTimeoutWorker",
};

constexpr std::array<std::wstring_view, 2> kComctl32ModalApis = {
  L"TaskDialog",
  L"TaskDialogIndirect",
};

// Error dialogs often quote file paths. Reports get shared, so drop the
// account name from user-profile paths the same way module paths are redacted.
std::wstring RedactUserProfileSegments(const std::wstring& text)
{
  const std::wstring lower = LowerAscii(text);
  constexpr std::wstring_view kMarkers[] = { L"\\users\\", L"/users/" };
  std::wstring out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t hit = std::wstring::npos;
    std::size_t markerLen = 0;
    for (const auto marker : kMarkers) {
      const auto found = lower.find(marker, pos);
      if (found != std::wstring::npos && (hit == std::wstring::npos || found < hit)) {
        hit = found;
        markerLen = marker.size();
      }
    }
    if (hit == std::wstring::npos) {
      out.append(text, pos, std::wstring::npos);
      break;
    }
    const std::size_t nameStart = hit + markerLen;
    out.append(text, pos, nameStart - pos);
    std::size_t nameEnd = nameStart;
    while (nameEnd < text.size() && text[nameEnd] != L'\\' && text[nameEnd] != L'/' &&
           text[nameEnd] != L'\n' && text[nameEnd] != L'"') {
      ++nameEnd;
    }
    if (nameEnd > nameStart) {
      out += L"<user>";
    }
    pos = nameEnd;
  }
  return out;
}

// Reports and recommendations are single-line fields.
std::wstring FlattenLines(const std::wstring& text)
{
  std::wstring out;
  out.reserve(text.size());
  bool pendingBreak = false;
  for (const wchar_t ch : text) {
    if (ch == L'\r' || ch == L'\n') {
      pendingBreak = !out.empty();
      continue;
    }
    if (pendingBreak) {
      out += L" / ";
      pendingBreak = false;
    }
    out.push_back(ch);
  }
  return out;
}

template <std::size_t N>
bool ContainsName(const std::array<std::wstring_view, N>& names, std::wstring_view symbol)
{
  return std::find(names.begin(), names.end(), symbol) != names.end();
}

}  // namespace

bool IsModalDialogApiFrame(const ModalStackFrame& frame)
{
  if (!frame.has_module || frame.symbol.empty() || frame.displacement > kModalApiMaxDisplacement) {
    return false;
  }
  const auto module = LowerAscii(frame.module_filename);
  if (module == L"user32.dll") {
    return ContainsName(kUser32ModalApis, frame.symbol);
  }
  if (module == L"comctl32.dll") {
    return ContainsName(kComctl32ModalApis, frame.symbol);
  }
  return false;
}

ModalStackMatch MatchModalDialogWaitStack(const std::vector<ModalStackFrame>& frames)
{
  ModalStackMatch match{};
  const std::size_t limit = std::min(frames.size(), kModalApiMaxDepth);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& frame = frames[i];
    // Unknown or non-system code above the modal API means something other
    // than the dialog's own wait is executing; do not classify it as modal.
    if (!frame.has_module || !frame.is_system) {
      break;
    }
    if (IsModalDialogApiFrame(frame)) {
      match.matched = true;
      match.api_frame_index = i;
      match.wait_api = frame.module_filename + L"!" + frame.symbol;
    }
  }
  if (!match.matched) {
    return match;
  }

  // The dialog owner is the first non-system caller below the modal API.
  // System callers (for example a CRT abort() dialog) are skipped, but the
  // walk stops at the first non-system module and never promotes past it.
  for (std::size_t i = match.api_frame_index + 1u; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module) {
      break;
    }
    if (frame.is_system) {
      continue;
    }
    match.has_caller = true;
    match.caller_frame_index = i;
    break;
  }
  return match;
}

std::string ClassifyModalDialogCaller(const ModalStackFrame& frame)
{
  if (frame.is_game_exe) {
    return "game_exe";
  }
  if (frame.is_skse_runtime) {
    return "skse_runtime";
  }
  if (frame.is_hook_framework) {
    return "hook_framework";
  }
  return "plugin";
}

ModalDialogWaitInfo ResolveModalDialogWait(const ModalDialogWaitInput& input)
{
  ModalDialogWaitInfo info{};
  if (input.main_thread_id == 0u) {
    return info;
  }
  info.main_thread_id = input.main_thread_id;

  if (input.wct) {
    const internal::WctModalDialog* selected = nullptr;
    for (const auto& dialog : input.wct->modal_dialogs) {
      if (dialog.tid != input.main_thread_id) {
        ++info.other_thread_dialog_count;
        continue;
      }
      // Prefer the dialog that disabled its owner: that one runs the modal loop.
      if (!selected || (!selected->owner_disabled && dialog.owner_disabled)) {
        selected = &dialog;
      }
    }
    if (selected) {
      info.window_evidence = true;
      info.dialog_title = FlattenLines(RedactUserProfileSegments(Utf8ToWide(selected->title)));
      info.dialog_text = FlattenLines(RedactUserProfileSegments(Utf8ToWide(selected->text)));
    }
  }

  if (input.main_thread_frames) {
    const auto match = MatchModalDialogWaitStack(*input.main_thread_frames);
    if (match.matched) {
      info.stack_evidence = true;
      info.wait_api = match.wait_api;
      if (match.has_caller) {
        const auto& caller = (*input.main_thread_frames)[match.caller_frame_index];
        info.caller_module_filename = caller.module_filename;
        info.caller_module_path = caller.module_path;
        info.caller_inferred_mod_name = caller.inferred_mod_name;
        info.caller_kind = ClassifyModalDialogCaller(caller);
      }
    }
  }

  info.detected = info.window_evidence || info.stack_evidence;
  return info;
}

}  // namespace skydiag::dump_tool
