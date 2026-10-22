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

bool IsAsciiAlnum(wchar_t ch)
{
  return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
}

// Characters that continue a file name on either side of a quoted DLL name.
// Non-ASCII ones do not: a Korean particle may follow the name directly.
bool IsFileNameChar(wchar_t ch)
{
  return IsAsciiAlnum(ch) || ch == L'_' || ch == L'-' || ch == L'+' || ch == L'.';
}

// A name ends where no word character follows; "SmoothCam.dll." at the end of
// a sentence still ends at the DLL name.
bool EndsNameAt(std::wstring_view text, std::size_t end)
{
  if (end >= text.size()) {
    return true;
  }
  const wchar_t next = text[end];
  if (next == L'.') {
    return end + 1u >= text.size() || !IsFileNameChar(text[end + 1u]);
  }
  return !IsFileNameChar(next);
}

// The whole file name appears in the text (both already lowercased).
bool ContainsFileNameToken(std::wstring_view lowerText, std::wstring_view lowerName)
{
  if (lowerName.empty()) {
    return false;
  }
  std::size_t pos = lowerText.find(lowerName);
  while (pos != std::wstring_view::npos) {
    const bool startsName = pos == 0u || !IsFileNameChar(lowerText[pos - 1u]);
    if (startsName && EndsNameAt(lowerText, pos + lowerName.size())) {
      return true;
    }
    pos = lowerText.find(lowerName, pos + 1u);
  }
  return false;
}

// "Foo.dll" names quoted in the text (a path keeps only its file name).
void AppendQuotedDllNames(std::wstring_view text, std::vector<std::wstring>* out)
{
  const std::wstring lower = LowerAscii(text);
  constexpr std::wstring_view kDll = L".dll";
  std::size_t pos = lower.find(kDll);
  while (pos != std::wstring::npos) {
    const std::size_t end = pos + kDll.size();
    if (EndsNameAt(lower, end)) {
      std::size_t start = pos;
      while (start > 0u && IsFileNameChar(text[start - 1u])) {
        --start;
      }
      while (start < pos && text[start] == L'.') {
        ++start;
      }
      if (start < pos) {
        std::wstring name(text.substr(start, end - start));
        const std::wstring lowerName = LowerAscii(name);
        const bool seen = std::any_of(out->begin(), out->end(),
          [&](const std::wstring& existing) { return LowerAscii(existing) == lowerName; });
        if (!seen) {
          out->push_back(std::move(name));
        }
      }
    }
    pos = lower.find(kDll, pos + 1u);
  }
}

// Loaded mod modules (not system, not the game) whose file name the text names.
std::vector<const ModalLoadedModule*> FindNamedLoadedModules(
  std::wstring_view text,
  const std::vector<ModalLoadedModule>& modules)
{
  std::vector<const ModalLoadedModule*> out;
  const std::wstring lowerText = LowerAscii(text);
  for (const auto& module : modules) {
    if (module.is_system || module.is_game_exe || module.filename.empty()) {
      continue;
    }
    const std::wstring lowerName = LowerAscii(module.filename);
    const bool seen = std::any_of(out.begin(), out.end(),
      [&](const ModalLoadedModule* existing) { return LowerAscii(existing->filename) == lowerName; });
    if (!seen && ContainsFileNameToken(lowerText, lowerName)) {
      out.push_back(&module);
    }
  }
  return out;
}

std::wstring TrimSpaces(std::wstring_view text)
{
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
    ++begin;
  }
  while (end > begin && (text[end - 1u] == L' ' || text[end - 1u] == L'\t')) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

// With no plugin caller on the stack, the dialog's own words name the mod.
void ResolveDialogNamedModule(const std::vector<ModalLoadedModule>& modules, ModalDialogWaitInfo* info)
{
  const auto inTitle = FindNamedLoadedModules(info->dialog_title, modules);
  const auto inText = inTitle.empty() ? FindNamedLoadedModules(info->dialog_text, modules)
                                      : std::vector<const ModalLoadedModule*>{};
  const auto& matches = inTitle.empty() ? inText : inTitle;
  if (matches.size() == 1u) {
    const auto& module = *matches.front();
    info->named_module_filename = module.filename;
    info->named_mod_name = module.inferred_mod_name;
    info->named_module_source = inTitle.empty() ? "text" : "title";
    info->named_module_version_issue =
      !info->address_library_issue.empty() || DialogTextReportsUnsupportedGameVersion(info->dialog_text);
    const bool exactTitle = LowerAscii(TrimSpaces(info->dialog_title)) == LowerAscii(module.filename);
    info->named_module_level = (exactTitle || info->named_module_version_issue) ? i18n::ConfidenceLevel::kHigh
                                                                                : i18n::ConfidenceLevel::kMedium;
    return;
  }
  for (const auto* module : matches) {
    info->named_loaded_modules.push_back(module->filename);
  }
  if (!matches.empty()) {
    return;
  }
  std::vector<std::wstring> quoted;
  AppendQuotedDllNames(info->dialog_title, &quoted);
  AppendQuotedDllNames(info->dialog_text, &quoted);
  for (auto& name : quoted) {
    const std::wstring lowerName = LowerAscii(name);
    // A loaded system or game module is no mod and is not "missing" either.
    const bool loaded = std::any_of(modules.begin(), modules.end(),
      [&](const ModalLoadedModule& module) { return LowerAscii(module.filename) == lowerName; });
    if (!loaded) {
      info->named_unloaded_modules.push_back(std::move(name));
    }
  }
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

std::string ClassifyAddressLibraryDialogText(std::wstring_view text)
{
  // REL::IDDatabase texts from CommonLibSSE and CommonLibSSE-NG. They are
  // English in every game language because the plugin formats them itself.
  constexpr std::wstring_view kPluginIncompatible[] = {
    L"unsupported address library format",
    L"failed to find the id within the address library",
  };
  constexpr std::wstring_view kAddressLibraryMissing[] = {
    L"failed to locate an appropriate address library",
    L"failed to open address library file",
  };
  const auto lower = LowerAscii(text);
  for (const auto needle : kPluginIncompatible) {
    if (lower.find(needle) != std::wstring::npos) {
      return "plugin_incompatible";
    }
  }
  for (const auto needle : kAddressLibraryMissing) {
    if (lower.find(needle) != std::wstring::npos) {
      return "address_library_missing";
    }
  }
  return {};
}

bool DialogTextReportsUnsupportedGameVersion(std::wstring_view text)
{
  if (!ClassifyAddressLibraryDialogText(text).empty()) {
    return true;
  }
  const auto lower = LowerAscii(text);
  const auto has = [&](std::wstring_view needle) { return lower.find(needle) != std::wstring::npos; };
  const bool unsupported = has(L"not support") || has(L"unsupported") || has(L"incompatible") ||
    has(L"not compatible");
  return unsupported && (has(L"version") || has(L"address library"));
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
      info.address_library_issue = ClassifyAddressLibraryDialogText(info.dialog_text);
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

  // A plugin or hook framework caller already names the opener.
  if (input.loaded_modules && info.window_evidence && info.caller_kind != "plugin" &&
      info.caller_kind != "hook_framework") {
    ResolveDialogNamedModule(*input.loaded_modules, &info);
  }

  info.detected = info.window_evidence || info.stack_evidence;
  return info;
}

}  // namespace skydiag::dump_tool
