#include "CrashLoggerParseCore.h"
#include "ThreadEntryHooks.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <unordered_map>

namespace skydiag::dump_tool {
namespace crashlogger_core {

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

CrashLoggerArtifactKind ClassifyCrashLoggerArtifactNameAscii(std::string_view filename)
{
  const auto slash = filename.find_last_of("/\\");
  if (slash != std::string_view::npos) {
    filename.remove_prefix(slash + 1);
  }
  const std::string lower = AsciiLower(filename);
  if (lower.starts_with("threaddump-") || lower.starts_with("threaddump_") ||
      lower.starts_with("thread-dump-") || lower.starts_with("thread_dump_")) {
    return CrashLoggerArtifactKind::kThreadDump;
  }
  if (lower.starts_with("crash-") || lower.starts_with("crash_")) {
    return CrashLoggerArtifactKind::kCrash;
  }
  return CrashLoggerArtifactKind::kUnknown;
}

bool IsBetterCrashLoggerPairCandidate(
  bool hasBest,
  std::uint64_t candidateDiff,
  CrashLoggerArtifactKind candidateKind,
  bool candidateSameDirectory,
  std::wstring_view candidateStablePath,
  std::uint64_t bestDiff,
  CrashLoggerArtifactKind bestKind,
  bool bestSameDirectory,
  std::wstring_view bestStablePath)
{
  if (!hasBest) {
    return true;
  }
  if (candidateDiff != bestDiff) {
    return candidateDiff < bestDiff;
  }
  const bool candidateKindKnown = candidateKind != CrashLoggerArtifactKind::kUnknown;
  const bool bestKindKnown = bestKind != CrashLoggerArtifactKind::kUnknown;
  if (candidateKindKnown != bestKindKnown) {
    return candidateKindKnown;
  }
  if (candidateSameDirectory != bestSameDirectory) {
    return candidateSameDirectory;
  }
  return candidateStablePath < bestStablePath;
}

std::optional<std::string> ParseCrashLoggerVersionAscii(std::string_view logUtf8)
{
  std::istringstream iss{ std::string(logUtf8) };
  std::string line;

  // The first line is typically: "CrashLoggerSSE vX.Y.Z"
  for (int i = 0; i < 32 && std::getline(iss, line); i++) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    const std::string lower = AsciiLower(line);
    const auto clPos = lower.find("crashloggersse");
    if (clPos == std::string::npos) {
      continue;
    }

    const auto vPos = lower.find('v', clPos);
    if (vPos == std::string::npos || vPos + 1 >= lower.size()) {
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(lower[vPos + 1]))) {
      continue;
    }

    std::size_t end = vPos + 1;
    while (end < lower.size()) {
      const unsigned char c = static_cast<unsigned char>(lower[end]);
      if (!(std::isdigit(c) || c == '.' || c == '-' || std::isalpha(c))) {
        break;
      }
      end++;
    }

    if (end <= vPos + 1) {
      continue;
    }
    return line.substr(vPos, end - vPos);
  }

  return std::nullopt;
}

std::string_view TrimLeftAscii(std::string_view s)
{
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c != ' ' && c != '\t') {
      break;
    }
    i++;
  }
  return s.substr(i);
}

std::string_view TrimRightAscii(std::string_view s)
{
  std::size_t end = s.size();
  while (end > 0) {
    const char c = s[end - 1];
    if (c != ' ' && c != '\t') {
      break;
    }
    end--;
  }
  return s.substr(0, end);
}

std::string_view TrimAscii(std::string_view s)
{
  return TrimRightAscii(TrimLeftAscii(s));
}

bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle)
{
  if (needle.empty()) {
    return true;
  }
  const std::string h = AsciiLower(haystack);
  const std::string n = AsciiLower(needle);
  return h.find(n) != std::string::npos;
}

bool StartsWithCaseInsensitiveAscii(std::string_view s, std::string_view prefix)
{
  if (prefix.empty()) {
    return true;
  }
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); i++) {
    const unsigned char a = static_cast<unsigned char>(s[i]);
    const unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

bool EqualsCaseInsensitiveAscii(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  return StartsWithCaseInsensitiveAscii(a, b);
}

std::optional<std::string> ParseCrashLoggerIniCrashlogDirectoryAscii(std::string_view iniUtf8)
{
  // Read the way Crash Logger reads it (SimpleIni) and the helper reads it
  // (ManualCaptureHotkey.h ParseCrashLoggerCrashlogDirectory): a UTF-8 BOM is
  // skipped, only lines that start with ';' or '#' are comments (a '#' or ';'
  // inside a value is part of the folder name), the last "Crashlog
  // Directory" under [Debug] wins, and a pair of surrounding double quotes is
  // dropped. An empty value means unset.
  if (iniUtf8.size() >= 3 && iniUtf8.substr(0, 3) == "\xEF\xBB\xBF") {
    iniUtf8.remove_prefix(3);
  }

  std::optional<std::string> found;
  bool inDebug = false;
  while (!iniUtf8.empty()) {
    const std::size_t eol = iniUtf8.find('\n');
    std::string_view line = iniUtf8.substr(0, eol);
    iniUtf8.remove_prefix(eol == std::string_view::npos ? iniUtf8.size() : eol + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    const std::string_view s = TrimAscii(line);
    if (s.empty() || s.front() == ';' || s.front() == '#') {
      continue;
    }

    if (s.front() == '[') {
      const auto close = s.find(']');
      if (close != std::string_view::npos) {
        inDebug = EqualsCaseInsensitiveAscii(TrimAscii(s.substr(1, close - 1)), "debug");
      }
      continue;
    }

    const auto eq = s.find('=');
    if (!inDebug || eq == std::string_view::npos) {
      continue;
    }
    if (!EqualsCaseInsensitiveAscii(TrimAscii(s.substr(0, eq)), "crashlog directory")) {
      continue;
    }

    std::string_view val = TrimAscii(s.substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = TrimAscii(val.substr(1, val.size() - 2));
    }
    found = std::string(val);
  }

  if (!found || found->empty()) {
    return std::nullopt;
  }
  return found;
}

bool IsSystemishModuleAsciiLower(std::string_view filenameLower)
{
  const char* k[] = {
    // Core Windows
    "kernelbase.dll", "ntdll.dll",       "kernel32.dll",      "ucrtbase.dll",
    "msvcp140.dll",   "vcruntime140.dll","vcruntime140_1.dll","concrt140.dll",
    "user32.dll",     "gdi32.dll",       "combase.dll",       "ole32.dll",
    "ws2_32.dll",     "win32u.dll",      "advapi32.dll",      "rpcrt4.dll",
    "sechost.dll",
    // Graphics / DirectX runtime
    "d3d11.dll",      "d3d12.dll",       "dxgi.dll",          "d3d9.dll",
    "opengl32.dll",   "d3dcompiler_47.dll", "dxcore.dll",
    // Debugging
    "dbghelp.dll",    "dbgcore.dll",
  };
  for (const auto* m : k) {
    if (filenameLower == m) {
      return true;
    }
  }
  return false;
}

bool IsGameExeModuleAsciiLower(std::string_view filenameLower)
{
  return filenameLower == "skyrimse.exe" || filenameLower == "skyrimae.exe" ||
         filenameLower == "skyrimvr.exe" || filenameLower == "skyrim.exe";
}

bool LooksLikeCrashLoggerLogTextCore(std::string_view utf8Prefix)
{
  if (!ContainsCaseInsensitiveAscii(utf8Prefix, "crashlogger")) {
    return false;
  }
  return ContainsCaseInsensitiveAscii(utf8Prefix, "crash time:") ||
         (ContainsCaseInsensitiveAscii(utf8Prefix, "thread dump") &&
          ContainsCaseInsensitiveAscii(utf8Prefix, "callstack:")) ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "probable call stack") ||
         (ContainsCaseInsensitiveAscii(utf8Prefix, "call stack") &&
          ContainsCaseInsensitiveAscii(utf8Prefix, "[p]robable") &&
          ContainsCaseInsensitiveAscii(utf8Prefix, "[s]tack scan")) ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "process info:");
}

std::optional<std::string_view> TryExtractModulePlusOffsetTokenAscii(std::string_view line)
{
  const auto lower = AsciiLower(line);

  auto pos = lower.find(".dll+");
  std::size_t plusLen = 5;
  if (pos == std::string::npos) {
    pos = lower.find(".exe+");
  }
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t start = pos;
  while (start > 0) {
    const char c = line[start - 1];
    if (c == ' ' || c == '\t') {
      break;
    }
    start--;
  }

  std::size_t end = pos + plusLen;
  while (end < line.size()) {
    const unsigned char c = static_cast<unsigned char>(line[end]);
    if (!std::isxdigit(c)) {
      break;
    }
    end++;
  }

  if (end <= start) {
    return std::nullopt;
  }
  return line.substr(start, end - start);
}

namespace {

bool IsModuleTokenCharAscii(char c)
{
  const unsigned char uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) || c == '.' || c == '_' || c == '-';
}

bool EndsWithCaseInsensitiveAscii(std::string_view s, std::string_view suffix)
{
  if (s.size() < suffix.size()) {
    return false;
  }
  return EqualsCaseInsensitiveAscii(s.substr(s.size() - suffix.size()), suffix);
}

std::string_view ExtractModuleFilenameAscii(std::string_view module)
{
  const auto pos = module.find_last_of("\\/:");
  if (pos == std::string_view::npos || pos + 1 >= module.size()) {
    return module;
  }
  return module.substr(pos + 1);
}

std::optional<std::string_view> TryExtractBareModuleTokenAscii(std::string_view line)
{
  for (std::size_t i = 0; i < line.size();) {
    if (!IsModuleTokenCharAscii(line[i])) {
      i++;
      continue;
    }

    std::size_t end = i + 1;
    while (end < line.size() && IsModuleTokenCharAscii(line[end])) {
      end++;
    }

    const std::string_view token = line.substr(i, end - i);
    if (EndsWithCaseInsensitiveAscii(token, ".dll") || EndsWithCaseInsensitiveAscii(token, ".exe")) {
      return token;
    }
    i = end;
  }

  return std::nullopt;
}

std::optional<std::string> TryExtractModuleNameFromLineAscii(std::string_view line)
{
  if (auto tokOpt = TryExtractModulePlusOffsetTokenAscii(line)) {
    const auto plus = tokOpt->find('+');
    if (plus != std::string_view::npos && plus > 0) {
      const std::string_view module = ExtractModuleFilenameAscii(tokOpt->substr(0, plus));
      return std::string(module);
    }
  }

  if (auto tokOpt = TryExtractBareModuleTokenAscii(line)) {
    const std::string_view module = ExtractModuleFilenameAscii(*tokOpt);
    return std::string(module);
  }

  return std::nullopt;
}

// The address written right before the module+offset token
// ("[ 0] 0x7FF612340001 d3d11.dll+0012345", "at 0x7FF612345678 d3d11.dll+..."),
// or 0.
std::uint64_t TryExtractFrameAddressAscii(std::string_view line)
{
  const auto token = TryExtractModulePlusOffsetTokenAscii(line);
  if (!token) {
    return 0;
  }
  std::size_t end = static_cast<std::size_t>(token->data() - line.data());
  while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
    end--;
  }
  std::size_t start = end;
  while (start > 0 && std::isxdigit(static_cast<unsigned char>(line[start - 1]))) {
    start--;
  }
  if (start == end || start < 2 || line[start - 2] != '0' || (line[start - 1] != 'x' && line[start - 1] != 'X')) {
    return 0;
  }
  std::uint64_t address = 0;
  const auto [ptr, ec] = std::from_chars(line.data() + start, line.data() + end, address, 16);
  return (ec == std::errc{} && ptr == line.data() + end) ? address : 0;
}

bool IsCrashLoggerProbableSectionTerminator(std::string_view line)
{
  const std::string_view trimmed = TrimAscii(line);
  if (trimmed.empty()) {
    return true;
  }

  return EqualsCaseInsensitiveAscii(trimmed, "REGISTERS:") ||
         EqualsCaseInsensitiveAscii(trimmed, "MODULES:") ||
         EqualsCaseInsensitiveAscii(trimmed, "POSSIBLE RELEVANT OBJECTS:") ||
         EqualsCaseInsensitiveAscii(trimmed, "C++ EXCEPTION:");
}

bool IsCrashLoggerMixedCallstackHeaderAscii(std::string_view line)
{
  const std::string_view trimmed = TrimAscii(line);
  return ContainsCaseInsensitiveAscii(trimmed, "call stack") &&
         ContainsCaseInsensitiveAscii(trimmed, "[p]robable") &&
         ContainsCaseInsensitiveAscii(trimmed, "[s]tack scan");
}

bool IsCrashLoggerProbableCallstackHeaderAscii(std::string_view line)
{
  return ContainsCaseInsensitiveAscii(line, "probable call stack") ||
         IsCrashLoggerMixedCallstackHeaderAscii(line);
}

bool IsCrashLoggerMixedProbableRowAscii(std::string_view line)
{
  return ContainsCaseInsensitiveAscii(TrimAscii(line), "[p]");
}

bool IsCrashLoggerDirectFaultLineAscii(std::string_view line)
{
  const std::string_view trimmed = TrimAscii(line);
  if (trimmed.empty()) {
    return false;
  }

  if (ContainsCaseInsensitiveAscii(trimmed, "unhandled exception")) {
    return true;
  }

  return StartsWithCaseInsensitiveAscii(trimmed, "fault module:") ||
         StartsWithCaseInsensitiveAscii(trimmed, "fault module name:") ||
         StartsWithCaseInsensitiveAscii(trimmed, "fault module path:") ||
         StartsWithCaseInsensitiveAscii(trimmed, "faulting module:") ||
         StartsWithCaseInsensitiveAscii(trimmed, "faulting module name:") ||
         StartsWithCaseInsensitiveAscii(trimmed, "faulting module path:");
}

bool IsCrashLoggerThreadDumpHeaderLineAscii(std::string_view line)
{
  const std::string_view trimmed = TrimAscii(line);
  if (EqualsCaseInsensitiveAscii(trimmed, "THREAD DUMP")) {
    return true;
  }

  if (!StartsWithCaseInsensitiveAscii(trimmed, "THREAD DUMP (")) {
    return false;
  }

  return trimmed.size() > std::string_view("THREAD DUMP ()").size() &&
         trimmed.back() == ')';
}

bool HasCrashLoggerThreadDumpHeaderAscii(std::string_view logUtf8)
{
  std::istringstream iss{ std::string(logUtf8) };
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (IsCrashLoggerThreadDumpHeaderLineAscii(line)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool IsCrashLoggerThreadDumpLogAscii(std::string_view logUtf8)
{
  return HasCrashLoggerThreadDumpHeaderAscii(logUtf8);
}

CrashLoggerThreadDumpProgress ParseCrashLoggerThreadDumpProgressAscii(std::string_view logUtf8)
{
  CrashLoggerThreadDumpProgress progress{};
  std::size_t pos = 0;
  while (pos < logUtf8.size()) {
    std::size_t end = logUtf8.find('\n', pos);
    if (end == std::string_view::npos) {
      end = logUtf8.size();
    }
    std::string_view raw = logUtf8.substr(pos, end - pos);
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }
    const std::string_view line = TrimAscii(raw);
    pos = end + 1u;
    if (!progress.is_thread_dump) {
      progress.is_thread_dump = IsCrashLoggerThreadDumpHeaderLineAscii(line);
      continue;
    }
    if (StartsWithCaseInsensitiveAscii(line, "===== THREAD ")) {
      ++progress.thread_records;
      if (ContainsCaseInsensitiveAscii(line, "[CURRENT THREAD]")) {
        progress.has_current_thread = true;
      }
      continue;
    }
    constexpr std::string_view kTotal = "Total Threads:";
    if (progress.thread_records == 0u && StartsWithCaseInsensitiveAscii(line, kTotal)) {
      const std::string_view value = TrimAscii(line.substr(kTotal.size()));
      std::uint32_t total = 0;
      bool digits = !value.empty();
      for (const char ch : value) {
        if (ch < '0' || ch > '9' || total > 100'000u) {
          digits = false;
          break;
        }
        total = total * 10u + static_cast<std::uint32_t>(ch - '0');
      }
      if (digits) {
        progress.total_threads = total;
      }
    }
  }
  return progress;
}

std::optional<CrashLoggerCppExceptionDetails> ParseCrashLoggerCppExceptionDetailsAscii(std::string_view logUtf8)
{
  std::istringstream iss{ std::string(logUtf8) };
  std::string line;

  bool inBlock = false;
  CrashLoggerCppExceptionDetails out{};
  bool gotAny = false;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (!inBlock) {
      if (ContainsCaseInsensitiveAscii(line, "c++ exception:")) {
        inBlock = true;
      }
      continue;
    }

    if (line.empty()) {
      break;
    }

    const char c0 = line.empty() ? '\0' : line[0];
    if (c0 != '\t' && c0 != ' ') {
      break;
    }

    const std::string_view trimmed = TrimLeftAscii(line);

    auto tryField = [&](std::string_view key, std::string& dst) {
      if (!StartsWithCaseInsensitiveAscii(trimmed, key)) {
        return false;
      }
      const std::string_view v = TrimAscii(trimmed.substr(key.size()));
      dst.assign(v.begin(), v.end());
      gotAny = true;
      return true;
    };

    if (tryField("Type:", out.type)) {
      continue;
    }
    if (tryField("Info:", out.info)) {
      continue;
    }
    if (tryField("Throw Location:", out.throw_location)) {
      continue;
    }
    if (tryField("Module:", out.module)) {
      continue;
    }
  }

  if (!gotAny) {
    return std::nullopt;
  }
  return out;
}

CrashLoggerFrameSignals ParseCrashLoggerFrameSignalsAscii(std::string_view logUtf8)
{
  return ParseCrashLoggerFrameSignalsAscii(logUtf8, CrashLoggerSystemNamedModFn{});
}

CrashLoggerFrameSignals ParseCrashLoggerFrameSignalsAscii(
  std::string_view logUtf8,
  const CrashLoggerSystemNamedModFn& isSystemNamedMod)
{
  CrashLoggerFrameSignals out{};
  if (logUtf8.empty()) {
    return out;
  }
  // A frame named like a Windows DLL in a mod's copy (ENB/ReShade d3d11.dll in
  // the game folder) is a plugin frame (review H4).
  const auto inSystemNamedMod = [&](std::string_view lineText, const std::string& module) {
    return isSystemNamedMod && IsSystemishModuleAsciiLower(AsciiLower(module)) &&
           isSystemNamedMod(module, TryExtractFrameAddressAscii(lineText));
  };

  std::istringstream iss{ std::string(logUtf8) };
  std::string line;
  bool inProbableCallStack = false;
  bool mixedProbableRows = false;
  bool allowDirectFaultScan = true;
  std::vector<StackFrameKind> rowKinds;  // every probable call-stack row
  std::vector<std::size_t> moduleRows;   // the row of each probable_modules_in_order entry

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (allowDirectFaultScan &&
        out.direct_fault_module.empty() &&
        IsCrashLoggerDirectFaultLineAscii(line)) {
      if (auto module = TryExtractModuleNameFromLineAscii(line)) {
        out.direct_fault_module = *module;
        out.direct_fault_in_system_named_mod = inSystemNamedMod(line, *module);
      }
    }

    if (!inProbableCallStack) {
      if (IsCrashLoggerProbableCallstackHeaderAscii(line)) {
        inProbableCallStack = true;
        mixedProbableRows = IsCrashLoggerMixedCallstackHeaderAscii(line);
        allowDirectFaultScan = false;
      }
      continue;
    }

    if (IsCrashLoggerProbableSectionTerminator(line)) {
      break;
    }

    if (mixedProbableRows && !IsCrashLoggerMixedProbableRowAscii(line)) {
      continue;
    }

    if (auto module = TryExtractModuleNameFromLineAscii(line)) {
      out.probable_modules_in_order.push_back(*module);
      const std::string moduleLower = AsciiLower(*module);
      const bool systemNamedMod = inSystemNamedMod(line, *module);
      out.probable_frame_in_system_named_mod.push_back(systemNamedMod);
      moduleRows.push_back(rowKinds.size());
      rowKinds.push_back(
        (IsSystemishModuleAsciiLower(moduleLower) && !systemNamedMod) ? StackFrameKind::kSystem
        : IsGameExeModuleAsciiLower(moduleLower)                      ? StackFrameKind::kGame
                                                                      : StackFrameKind::kPlugin);
    } else if (const auto trimmed = TrimAscii(line); !trimmed.empty() && trimmed.front() == '[') {
      // A frame in no module (JIT code, a freed DLL) keeps its place, so the
      // frames around it are not taken as adjacent (ThreadEntryHooks.h).
      rowKinds.push_back(StackFrameKind::kUnknown);
    }
  }

  // Marked per call-stack row, then kept for the rows that name a module. The
  // shape only means "thread entry" when the stack reaches the thread start
  // (RtlUserThreadStart in ntdll.dll); a stack Crash Logger cut short in some
  // other system DLL proves nothing about what lies below it.
  const bool reachesThreadStart = !rowKinds.empty() && !moduleRows.empty() &&
                                  moduleRows.back() == rowKinds.size() - 1 &&
                                  AsciiLower(out.probable_modules_in_order.back()) == "ntdll.dll";
  const auto rowMarked = reachesThreadStart ? MarkThreadEntryHookFrames(rowKinds)
                                            : std::vector<bool>(rowKinds.size(), false);
  std::vector<StackFrameKind> kinds;
  kinds.reserve(moduleRows.size());
  out.probable_frame_is_thread_entry_hook.reserve(moduleRows.size());
  for (const auto row : moduleRows) {
    kinds.push_back(rowKinds[row]);
    out.probable_frame_is_thread_entry_hook.push_back(rowMarked[row]);
  }
  out.thread_entry_hook_modules =
    ThreadEntryHookOnlyModules(out.probable_modules_in_order, out.probable_frame_is_thread_entry_hook);

  for (std::size_t i = 0; i < out.probable_modules_in_order.size(); ++i) {
    if (kinds[i] == StackFrameKind::kPlugin && !out.probable_frame_is_thread_entry_hook[i]) {
      out.first_actionable_probable_module = out.probable_modules_in_order[i];
      out.first_actionable_probable_in_system_named_mod = out.probable_frame_in_system_named_mod[i];
      break;
    }
  }

  std::string currentLower;
  std::string currentModule;
  bool currentInSystemNamedMod = false;
  std::uint32_t currentLength = 0;
  for (std::size_t i = 0; i < out.probable_modules_in_order.size(); ++i) {
    const auto& module = out.probable_modules_in_order[i];
    // A thread-entry hook is no streak (the Analyzer promotes this one).
    if (out.probable_frame_is_thread_entry_hook[i]) {
      currentLower.clear();
      currentModule.clear();
      currentLength = 0;
      continue;
    }
    // The mod's copy and the Windows DLL of the same name are no one streak.
    const bool inMod = out.probable_frame_in_system_named_mod[i];
    const std::string moduleLower = AsciiLower(module);
    if (moduleLower == currentLower && inMod == currentInSystemNamedMod) {
      currentLength++;
    } else {
      currentLower = moduleLower;
      currentModule = module;
      currentInSystemNamedMod = inMod;
      currentLength = 1;
    }

    if (currentLength > out.probable_streak_length) {
      out.probable_streak_length = currentLength;
      out.probable_streak_module = currentModule;
      out.probable_streak_in_system_named_mod = currentInSystemNamedMod;
    }
  }

  return out;
}

std::vector<std::string> ParseCrashLoggerFrameTopModulesAsciiLower(std::string_view logUtf8)
{
  std::unordered_map<std::string, std::uint32_t> freq;
  std::unordered_map<std::string, std::size_t> firstSeen;  // ties keep stack order
  const auto signals = ParseCrashLoggerFrameSignalsAscii(logUtf8);
  for (std::size_t i = 0; i < signals.probable_modules_in_order.size(); ++i) {
    const auto& module = signals.probable_modules_in_order[i];
    const std::string moduleLower = AsciiLower(module);
    if (IsSystemishModuleAsciiLower(moduleLower) || IsGameExeModuleAsciiLower(moduleLower) ||
        signals.probable_frame_is_thread_entry_hook[i]) {
      continue;
    }
    firstSeen.emplace(moduleLower, firstSeen.size());
    freq[moduleLower] += 1;
  }

  struct Row
  {
    std::string moduleLower;
    std::uint32_t count = 0;
    std::size_t first = 0;
  };

  std::vector<Row> rows;
  rows.reserve(freq.size());
  for (const auto& [k, v] : freq) {
    rows.push_back(Row{ k, v, firstSeen[k] });
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.first < b.first;
  });

  std::vector<std::string> out;
  out.reserve(std::min<std::size_t>(rows.size(), 8));
  for (const auto& row : rows) {
    out.push_back(row.moduleLower);
    if (out.size() >= 8) {
      break;
    }
  }

  return out;
}

std::vector<std::string> ParseCrashLoggerTopModulesAsciiLower(std::string_view logUtf8)
{
  const bool isThreadDump = HasCrashLoggerThreadDumpHeaderAscii(logUtf8);
  if (!isThreadDump) {
    return ParseCrashLoggerFrameTopModulesAsciiLower(logUtf8);
  }

  std::unordered_map<std::string, std::uint32_t> freq;
  std::unordered_map<std::string, std::size_t> firstSeen;  // ties keep stack order
  std::istringstream iss{ std::string(logUtf8) };
  std::string line;

  bool inThreadCallstack = false;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (!inThreadCallstack) {
      if (EqualsCaseInsensitiveAscii(TrimAscii(line), "CALLSTACK:")) {
        inThreadCallstack = true;
      }
      continue;
    }

    if (line.empty()) {
      inThreadCallstack = false;
      continue;
    }
    if (!line.empty() && line[0] == '=') {
      inThreadCallstack = false;
      continue;
    }

    auto moduleOpt = TryExtractModuleNameFromLineAscii(line);
    if (!moduleOpt) {
      continue;
    }
    const std::string moduleLower = AsciiLower(*moduleOpt);
    if (IsSystemishModuleAsciiLower(moduleLower) || IsGameExeModuleAsciiLower(moduleLower)) {
      continue;
    }
    firstSeen.emplace(moduleLower, firstSeen.size());
    freq[moduleLower] += 1;
  }

  struct Row
  {
    std::string moduleLower;
    std::uint32_t count = 0;
    std::size_t first = 0;
  };

  std::vector<Row> rows;
  rows.reserve(freq.size());
  for (const auto& [k, v] : freq) {
    rows.push_back(Row{ k, v, firstSeen[k] });
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.first < b.first;
  });

  std::vector<std::string> out;
  out.reserve(std::min<std::size_t>(rows.size(), 8));
  for (const auto& r : rows) {
    out.push_back(r.moduleLower);
    if (out.size() >= 8) {
      break;
    }
  }

  return out;
}

// Extract YYYYMMDD_HHMMSS from a filename stem.
std::optional<ParsedTimestamp> TryExtractCompactTimestampFromStem(std::wstring_view stem)
{
  const auto is_digits = [](std::wstring_view s) {
    for (wchar_t c : s) {
      if (c < L'0' || c > L'9') return false;
    }
    return true;
  };

  for (std::size_t i = 0; i + 15 <= stem.size(); i++) {
    const std::wstring_view date = stem.substr(i, 8);
    if (!is_digits(date)) continue;
    if (stem[i + 8] != L'_') continue;
    const std::wstring_view time = stem.substr(i + 9, 6);
    if (!is_digits(time)) continue;

    ParsedTimestamp ts{};
    ts.year   = std::stoi(std::wstring(stem.substr(i, 4)));
    ts.month  = std::stoi(std::wstring(stem.substr(i + 4, 2)));
    ts.day    = std::stoi(std::wstring(stem.substr(i + 6, 2)));
    ts.hour   = std::stoi(std::wstring(stem.substr(i + 9, 2)));
    ts.minute = std::stoi(std::wstring(stem.substr(i + 11, 2)));
    ts.second = std::stoi(std::wstring(stem.substr(i + 13, 2)));

    if (ts.month < 1 || ts.month > 12 || ts.day < 1 || ts.day > 31 ||
        ts.hour < 0 || ts.hour > 23 || ts.minute < 0 || ts.minute > 59 ||
        ts.second < 0 || ts.second > 59) continue;

    return ts;
  }
  return std::nullopt;
}

// Extract YYYY-MM-DD-HH-MM-SS from a filename stem (Crash Logger format).
std::optional<ParsedTimestamp> TryExtractDashedTimestampFromStem(std::wstring_view stem)
{
  const auto is_digits = [](std::wstring_view s) {
    for (wchar_t c : s) {
      if (c < L'0' || c > L'9') return false;
    }
    return true;
  };

  for (std::size_t i = 0; i + 19 <= stem.size(); i++) {
    const std::wstring_view v = stem.substr(i, 19);
    if (v[4] != L'-' || v[7] != L'-' || v[10] != L'-' || v[13] != L'-' || v[16] != L'-') continue;

    const std::wstring_view yS  = v.substr(0, 4);
    const std::wstring_view moS = v.substr(5, 2);
    const std::wstring_view dS  = v.substr(8, 2);
    const std::wstring_view hhS = v.substr(11, 2);
    const std::wstring_view mmS = v.substr(14, 2);
    const std::wstring_view ssS = v.substr(17, 2);
    if (!is_digits(yS) || !is_digits(moS) || !is_digits(dS) ||
        !is_digits(hhS) || !is_digits(mmS) || !is_digits(ssS)) continue;

    ParsedTimestamp ts{};
    ts.year   = std::stoi(std::wstring(yS));
    ts.month  = std::stoi(std::wstring(moS));
    ts.day    = std::stoi(std::wstring(dS));
    ts.hour   = std::stoi(std::wstring(hhS));
    ts.minute = std::stoi(std::wstring(mmS));
    ts.second = std::stoi(std::wstring(ssS));

    if (ts.month < 1 || ts.month > 12 || ts.day < 1 || ts.day > 31 ||
        ts.hour < 0 || ts.hour > 23 || ts.minute < 0 || ts.minute > 59 ||
        ts.second < 0 || ts.second > 59) continue;

    return ts;
  }
  return std::nullopt;
}

bool IsVanillaDlcEspAsciiLower(std::string_view espLower)
{
  if (espLower == "skyrim.esm" || espLower == "update.esm" ||
      espLower == "dawnguard.esm" || espLower == "hearthfires.esm" ||
      espLower == "dragonborn.esm") {
    return true;
  }
  if (espLower.size() >= 3 && espLower[0] == 'c' && espLower[1] == 'c') {
    return true;
  }
  return false;
}

std::uint32_t LocationWeight(std::string_view loc)
{
  const std::string lower = AsciiLower(loc);
  if (lower == "rcx" || lower == "rdx" || lower == "r8" || lower == "r9") return 12;
  if (lower == "rdi" || lower == "rsi") return 10;
  if (lower == "rax" || lower == "rbx") return 8;
  if (lower.size() == 3 && lower[0] == 'r' && lower[1] == '1' &&
      lower[2] >= '0' && lower[2] <= '5') return 6;
  if (lower.size() >= 4 && lower.substr(0, 3) == "rsp") {
    auto plusPos = lower.find('+');
    if (plusPos != std::string::npos && plusPos + 1 < lower.size()) {
      unsigned long offset = 0;
      try {
        offset = std::stoul(lower.substr(plusPos + 1), nullptr, 16);
      } catch (...) {
        return 3;
      }
      if (offset <= 0xFF) return 5;
      if (offset <= 0x3FF) return 3;
      return 1;
    }
    return 5;
  }
  return 3;
}

std::uint32_t TypeWeight(std::string_view objType)
{
  const std::string lower = AsciiLower(objType);
  if (lower.find("character") != std::string::npos ||
      lower.find("actor") != std::string::npos ||
      lower.find("tesnpc") != std::string::npos) return 8;
  if (lower.find("tesobjectrefr") != std::string::npos ||
      lower.find("tescell") != std::string::npos ||
      lower.find("tesworldspace") != std::string::npos) return 6;
  if (lower.find("tes") != std::string::npos) return 4;
  if (lower.find("bsfadenode") != std::string::npos ||
      lower.find("ninode") != std::string::npos) return 2;
  return 1;
}

bool LooksLikePluginExtension(std::string_view s)
{
  if (s.size() < 5) return false;
  const std::string lower = AsciiLower(s);
  return lower.size() >= 4 &&
    (lower.substr(lower.size() - 4) == ".esp" ||
     lower.substr(lower.size() - 4) == ".esm" ||
     lower.substr(lower.size() - 4) == ".esl");
}

std::string ExtractFormIdBefore(std::string_view line, std::size_t pos)
{
  if (pos == 0) return {};
  std::size_t p = pos;
  while (p > 0 && (line[p - 1] == ' ' || line[p - 1] == '\t')) --p;
  if (p == 0 || line[p - 1] != ']') return {};
  --p;
  auto bracket = line.rfind('[', p);
  if (bracket == std::string_view::npos || bracket >= p) return {};
  std::string_view inside = line.substr(bracket + 1, p - bracket - 1);
  while (!inside.empty() && (inside.front() == ' ' || inside.front() == '\t')) inside.remove_prefix(1);
  while (!inside.empty() && (inside.back() == ' ' || inside.back() == '\t')) inside.remove_suffix(1);
  if (inside.size() < 3 || inside[0] != '0' || (inside[1] != 'x' && inside[1] != 'X')) return {};
  for (std::size_t i = 2; i < inside.size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(inside[i]))) return {};
  }
  return std::string(inside);
}

std::vector<EspRefEntry> ExtractEspRefsFromLine(std::string_view line)
{
  std::vector<EspRefEntry> results;
  std::size_t pos = 0;

  while (pos < line.size()) {
    auto nextParen = line.find('(', pos);
    auto nextFile = line.find("File:", pos);
    if (nextFile == std::string_view::npos) nextFile = line.find("file:", pos);

    if (nextParen == std::string_view::npos && nextFile == std::string_view::npos) break;

    // Handle File: pattern if it comes before next paren
    if (nextFile != std::string_view::npos &&
        (nextParen == std::string_view::npos || nextFile < nextParen)) {
      auto fq = line.find('"', nextFile + 5);
      if (fq == std::string_view::npos) { pos = nextFile + 5; continue; }
      auto fqe = line.find('"', fq + 1);
      if (fqe == std::string_view::npos) { pos = fq + 1; continue; }
      std::string_view name = line.substr(fq + 1, fqe - (fq + 1));
      // Quoting already gives us an exact filename boundary. Spaces are valid
      // in Skyrim plugin filenames (for example "JK's Skyrim.esp").
      if (LooksLikePluginExtension(name)) {
        EspRefEntry entry;
        entry.esp_name = std::string(name);
        entry.form_id = ExtractFormIdBefore(line, nextFile);
        results.push_back(std::move(entry));
      }
      pos = fqe + 1;
      continue;
    }

    // Handle paren pattern
    auto cp = line.find(')', nextParen + 1);
    if (cp == std::string_view::npos) { pos = nextParen + 1; continue; }

    if (nextParen + 1 < line.size() && line[nextParen + 1] == '"') {
      auto endq = line.find("\")", nextParen + 2);
      if (endq != std::string_view::npos) {
        std::string_view name = line.substr(nextParen + 2, endq - (nextParen + 2));
        if (LooksLikePluginExtension(name)) {
          EspRefEntry entry;
          entry.esp_name = std::string(name);
          entry.form_id = ExtractFormIdBefore(line, nextParen);
          results.push_back(std::move(entry));
        }
        pos = endq + 2;
      } else {
        pos = nextParen + 2;
      }
      continue;
    }

    // Unquoted
    std::string_view inside = line.substr(nextParen + 1, cp - (nextParen + 1));
    while (!inside.empty() && (inside.front() == ' ' || inside.front() == '\t')) inside.remove_prefix(1);
    while (!inside.empty() && (inside.back() == ' ' || inside.back() == '\t')) inside.remove_suffix(1);
    if (LooksLikePluginExtension(inside) && inside.find(' ') == std::string_view::npos) {
      EspRefEntry entry;
      entry.esp_name = std::string(inside);
      entry.form_id = ExtractFormIdBefore(line, nextParen);
      results.push_back(std::move(entry));
    }
    pos = cp + 1;
  }

  return results;
}

std::vector<std::string> ExtractEspNamesFromLine(std::string_view line)
{
  auto refs = ExtractEspRefsFromLine(line);
  std::vector<std::string> results;
  results.reserve(refs.size());
  for (auto& r : refs) {
    results.push_back(std::move(r.esp_name));
  }
  return results;
}

std::string_view ObjectPayloadAfterLocation(std::string_view line)
{
  const auto colon = line.find(':');
  if (colon != std::string_view::npos) {
    line = line.substr(colon + 1);
  }
  return TrimLeftAscii(line);
}

std::string ExtractObjectType(std::string_view line)
{
  auto open = line.find('(');
  while (open != std::string_view::npos) {
    auto close = line.find(')', open + 1);
    if (close == std::string_view::npos) break;
    std::string_view inside = line.substr(open + 1, close - (open + 1));
    if (!inside.empty() && inside.back() == '*') {
      if (!LooksLikePluginExtension(inside)) {
        return std::string(inside);
      }
    }
    open = line.find('(', close + 1);
  }

  std::string_view payload = ObjectPayloadAfterLocation(line);
  if (payload.empty() || payload.front() == '(' || payload.front() == '[' || payload.front() == '"') {
    return {};
  }

  const auto typeEnd = payload.find_first_of(" \t\"[({");
  std::string_view typeName = typeEnd == std::string_view::npos ? payload : payload.substr(0, typeEnd);
  typeName = TrimAscii(typeName);
  if (typeName.empty() || LooksLikePluginExtension(typeName)) {
    return {};
  }
  return std::string(typeName);
}

std::string ExtractSimplifiedObjectName(std::string_view line)
{
  std::string_view payload = ObjectPayloadAfterLocation(line);
  if (payload.empty() || payload.front() == '(' || payload.front() == '[') {
    return {};
  }

  const auto quote = payload.find('"');
  if (quote == std::string_view::npos) {
    return {};
  }

  const auto bracket = payload.find('[');
  if (bracket != std::string_view::npos && bracket < quote) {
    return {};
  }

  const auto paren = payload.find('(');
  if (paren != std::string_view::npos && paren < quote) {
    return {};
  }

  const auto endq = payload.find('"', quote + 1);
  if (endq == std::string_view::npos) {
    return {};
  }

  return std::string(payload.substr(quote + 1, endq - quote - 1));
}

std::string ExtractObjectName(std::string_view line)
{
  auto typeEnd = line.find("*) ");
  if (typeEnd == std::string_view::npos) {
    return ExtractSimplifiedObjectName(line);
  }
  auto afterType = line.substr(typeEnd + 3);
  if (afterType.empty() || afterType[0] != '"') return {};
  auto endq = afterType.find('"', 1);
  if (endq == std::string_view::npos) return {};
  return std::string(afterType.substr(1, endq - 1));
}

std::string ExtractLocation(std::string_view trimmed)
{
  auto colon = trimmed.find(':');
  if (colon == std::string_view::npos || colon == 0) return {};
  std::string_view loc = trimmed.substr(0, colon);
  while (!loc.empty() && (loc.back() == ' ' || loc.back() == '\t')) loc.remove_suffix(1);
  return std::string(loc);
}

std::vector<std::string> ParseCrashLoggerObjectTypesAscii(std::string_view logUtf8)
{
  std::vector<std::string> types;
  std::istringstream iss{ std::string(logUtf8) };
  std::string line;
  enum class Section { kOther, kObjects, kRegisters, kCppException };
  Section section = Section::kOther;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const bool isHeader = !line.empty() && line.front() != '\t' && line.front() != ' ' && line.back() == ':';
    if (isHeader) {
      section = ContainsCaseInsensitiveAscii(line, "POSSIBLE RELEVANT OBJECTS") ? Section::kObjects
                : ContainsCaseInsensitiveAscii(line, "REGISTERS")               ? Section::kRegisters
                : ContainsCaseInsensitiveAscii(line, "C++ EXCEPTION")           ? Section::kCppException
                                                                                : Section::kOther;
      continue;
    }
    std::string type;
    if (section == Section::kCppException) {
      // The thrown object's type: "Type: (std::bad_alloc*)" (Crash Logger
      // 1.25), "Type: std::bad_alloc" (older). It is in no other section.
      const std::string_view trimmed = TrimAscii(line);
      if (!StartsWithCaseInsensitiveAscii(trimmed, "Type:")) {
        continue;
      }
      std::string_view value = TrimAscii(trimmed.substr(5));
      if (!value.empty() && value.front() == '(' && value.back() == ')') {
        value = TrimAscii(value.substr(1, value.size() - 2));
      }
      for (const std::string_view prefix : { std::string_view("class "), std::string_view("struct ") }) {
        if (StartsWithCaseInsensitiveAscii(value, prefix)) {
          value = TrimAscii(value.substr(prefix.size()));
        }
      }
      type.assign(value.begin(), value.end());
    } else if (section == Section::kObjects) {
      type = ExtractObjectType(TrimAscii(line));
    } else if (section == Section::kRegisters) {
      // Only a pointer type in parentheses ("RCX 0x... (NiTriShape*)"); the
      // fallback would take the register name of "RAX 0x0 (size_t) [0]".
      type = ExtractObjectType(TrimAscii(line));
      if (type.empty() || type.back() != '*') {
        continue;
      }
    } else {
      continue;
    }
    while (!type.empty() && (type.back() == '*' || type.back() == ' ')) {
      type.pop_back();
    }
    if (!type.empty() && std::find(types.begin(), types.end(), type) == types.end()) {
      types.push_back(std::move(type));
    }
  }
  return types;
}

namespace {

// Leading tabs (or groups of four spaces) before a Crash Logger log line.
std::size_t IndentDepth(std::string_view line)
{
  std::size_t depth = 0;
  std::size_t spaces = 0;
  for (const char c : line) {
    if (c == '\t') {
      ++depth;
      spaces = 0;
    } else if (c == ' ') {
      if (++spaces == 4) {
        ++depth;
        spaces = 0;
      }
    } else {
      break;
    }
  }
  return depth;
}

// "RBP 0x...", "R9  0x...", or the older "RDI: (...)": the register's name.
std::optional<std::string> RegisterNameAtLineStart(std::string_view trimmed)
{
  const auto end = trimmed.find_first_of(" \t:");
  const std::string name = AsciiLower(trimmed.substr(0, end));
  static constexpr std::string_view kRegisters[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
  };
  for (const auto reg : kRegisters) {
    if (name == reg) {
      std::string upper(trimmed.substr(0, end));
      for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      return upper;
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<CrashLoggerObjectRef> ParseCrashLoggerObjectRefsAscii(std::string_view logUtf8)
{
  std::vector<CrashLoggerObjectRef> results;
  if (logUtf8.empty()) return results;

  std::istringstream iss{std::string(logUtf8)};
  std::string line;

  enum class Section { kNone, kPossibleObjects, kRegisters };
  Section section = Section::kNone;
  std::string currentRegister;
  std::size_t registerDepth = 0;
  std::string registerType;
  std::string registerName;
  std::string registerFormId;
  std::size_t registerRefStart = 0;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Section detection
    if (ContainsCaseInsensitiveAscii(line, "POSSIBLE RELEVANT OBJECTS")) {
      section = Section::kPossibleObjects;
      continue;
    }
    if (ContainsCaseInsensitiveAscii(line, "REGISTERS:")) {
      section = Section::kRegisters;
      currentRegister.clear();
      continue;
    }
    // End of sections
    if (section != Section::kNone) {
      if (ContainsCaseInsensitiveAscii(line, "MODULES:") ||
          ContainsCaseInsensitiveAscii(line, "PROBABLE CALL STACK") ||
          ContainsCaseInsensitiveAscii(line, "STACK:") ||
          ContainsCaseInsensitiveAscii(line, "C++ EXCEPTION:")) {
        if (section == Section::kPossibleObjects) {
          section = Section::kRegisters;
          if (ContainsCaseInsensitiveAscii(line, "REGISTERS:")) {
            currentRegister.clear();
            continue;
          }
          section = Section::kNone;
        } else {
          section = Section::kNone;
        }
        continue;
      }
    }

    if (section == Section::kPossibleObjects) {
      const std::string_view trimmed = TrimLeftAscii(line);
      if (trimmed.empty()) continue;

      if (StartsWithCaseInsensitiveAscii(trimmed, "Modified by:")) {
        continue;
      }

      std::string location = ExtractLocation(trimmed);
      if (location.empty()) continue;

      std::string objType = ExtractObjectType(trimmed);
      std::string objName = ExtractObjectName(trimmed);

      auto espRefs = ExtractEspRefsFromLine(trimmed);
      if (espRefs.empty()) continue;

      for (const auto& entry : espRefs) {
        const std::string lower = AsciiLower(entry.esp_name);
        if (IsVanillaDlcEspAsciiLower(lower)) continue;

        CrashLoggerObjectRef ref;
        ref.location = location;
        ref.object_type = objType;
        ref.esp_name = entry.esp_name;
        ref.object_name = objName;
        ref.form_id = entry.form_id;
        ref.relevance_score = LocationWeight(location) + TypeWeight(objType);
        results.push_back(std::move(ref));
      }
    } else if (section == Section::kRegisters) {
      const std::string_view trimmed = TrimLeftAscii(line);
      if (trimmed.empty()) {
        section = Section::kNone;
        continue;
      }

      // Real logs indent register lines one tab ("\tRBP 0x... (TESObjectBOOK*)")
      // and the object's own fields one tab deeper ("\t\tFile: \"X.esp\"").
      // Deeper lines describe other objects it points to, such as the cell
      // under "ParentCell:" (where the player stood), so they are not this
      // register's plugin.
      const std::size_t depth = IndentDepth(line);
      if (const auto reg = RegisterNameAtLineStart(trimmed)) {
        currentRegister = *reg;
        registerDepth = depth;
        registerType = ExtractObjectType(trimmed);
        registerName = ExtractObjectName(trimmed);
        registerFormId.clear();
        registerRefStart = results.size();
        for (const auto& entry : ExtractEspRefsFromLine(trimmed)) {
          if (IsVanillaDlcEspAsciiLower(AsciiLower(entry.esp_name))) continue;
          CrashLoggerObjectRef ref;
          ref.location = currentRegister;
          ref.object_type = registerType;
          ref.esp_name = entry.esp_name;
          ref.object_name = registerName;
          ref.form_id = entry.form_id;
          ref.relevance_score = LocationWeight(currentRegister) + TypeWeight(registerType);
          results.push_back(std::move(ref));
        }
        continue;
      }
      if (currentRegister.empty() || depth != registerDepth + 1) {
        continue;
      }
      if (StartsWithCaseInsensitiveAscii(trimmed, "Modified by:")) {
        continue;
      }
      if (StartsWithCaseInsensitiveAscii(trimmed, "FormID:")) {
        registerFormId = std::string(TrimAscii(trimmed.substr(7)));
        for (std::size_t i = registerRefStart; i < results.size(); ++i) {
          if (results[i].form_id.empty()) {
            results[i].form_id = registerFormId;
          }
        }
        continue;
      }
      if (StartsWithCaseInsensitiveAscii(trimmed, "Name:")) {
        std::string_view value = TrimAscii(trimmed.substr(5));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        registerName = std::string(value);
        for (std::size_t i = registerRefStart; i < results.size(); ++i) {
          if (results[i].object_name.empty()) {
            results[i].object_name = registerName;
          }
        }
        continue;
      }
      for (const auto& entry : ExtractEspRefsFromLine(trimmed)) {
        if (IsVanillaDlcEspAsciiLower(AsciiLower(entry.esp_name))) continue;
        CrashLoggerObjectRef ref;
        ref.location = currentRegister;
        ref.object_type = registerType;
        ref.esp_name = entry.esp_name;
        ref.object_name = registerName;
        ref.form_id = entry.form_id.empty() ? registerFormId : entry.form_id;
        ref.relevance_score = LocationWeight(currentRegister) + TypeWeight(registerType);
        results.push_back(std::move(ref));
      }
    }
  }

  std::sort(results.begin(), results.end(), [](const CrashLoggerObjectRef& a, const CrashLoggerObjectRef& b) {
    return a.relevance_score > b.relevance_score;
  });

  return results;
}

std::vector<CrashLoggerObjectRef> AggregateCrashLoggerObjectRefs(
    const std::vector<CrashLoggerObjectRef>& refs)
{
  struct Group {
    std::string canonical_esp;
    std::string best_object_type;
    std::string best_location;
    std::string object_name;
    std::string best_form_id;
    std::uint32_t ref_count = 0;
    std::uint32_t max_score = 0;
  };

  std::unordered_map<std::string, std::size_t> indexByLower;
  std::vector<Group> groups;

  for (const auto& ref : refs) {
    const std::string lower = AsciiLower(ref.esp_name);
    auto it = indexByLower.find(lower);
    if (it == indexByLower.end()) {
      indexByLower[lower] = groups.size();
      Group g;
      g.canonical_esp = ref.esp_name;
      g.best_object_type = ref.object_type;
      g.best_location = ref.location;
      g.object_name = ref.object_name;
      g.best_form_id = ref.form_id;
      g.ref_count = 1;
      g.max_score = ref.relevance_score;
      groups.push_back(std::move(g));
    } else {
      auto& g = groups[it->second];
      g.ref_count++;
      if (ref.relevance_score > g.max_score) {
        g.max_score = ref.relevance_score;
        g.best_object_type = ref.object_type;
        g.best_location = ref.location;
        if (!ref.form_id.empty()) g.best_form_id = ref.form_id;
      }
      if (g.object_name.empty() && !ref.object_name.empty()) {
        g.object_name = ref.object_name;
      }
      if (g.best_form_id.empty() && !ref.form_id.empty()) {
        g.best_form_id = ref.form_id;
      }
    }
  }

  std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
    return a.max_score > b.max_score;
  });

  std::vector<CrashLoggerObjectRef> result;
  result.reserve(std::min<std::size_t>(groups.size(), 8));
  for (const auto& g : groups) {
    if (result.size() >= 8) break;
    CrashLoggerObjectRef r;
    r.location = g.best_location;
    r.object_type = g.best_object_type;
    r.esp_name = g.canonical_esp;
    r.object_name = g.object_name;
    r.form_id = g.best_form_id;
    r.relevance_score = g.max_score;
    result.push_back(std::move(r));
  }
  return result;
}

std::optional<CrashLoggerExceptionLine> ParseCrashLoggerExceptionLineAscii(std::string_view logUtf8)
{
  constexpr std::string_view kPrefix = "Unhandled exception \"";
  const std::size_t start = logUtf8.find(kPrefix);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t lineEnd = logUtf8.find('\n', start);
  std::string_view line = logUtf8.substr(start, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - start);
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  line.remove_prefix(kPrefix.size());
  const std::size_t quote = line.find('"');
  if (quote == std::string_view::npos || quote == 0) {
    return std::nullopt;
  }
  CrashLoggerExceptionLine out;
  out.type = std::string(line.substr(0, quote));
  std::string_view rest = line.substr(quote + 1);
  // " at 0x<address> <module>+<offset>\t<instruction>"
  const std::size_t at = rest.find(" at ");
  if (at != std::string_view::npos) {
    rest = TrimLeftAscii(rest.substr(at + 4));
    const std::size_t afterAddress = rest.find_first_of(" \t");
    if (afterAddress != std::string_view::npos) {
      rest = TrimLeftAscii(rest.substr(afterAddress));
      const std::size_t tab = rest.find('\t');
      out.location = std::string(TrimAscii(rest.substr(0, tab)));
      if (tab != std::string_view::npos) {
        out.instruction = std::string(TrimAscii(rest.substr(tab + 1)));
      }
    }
  }
  return out;
}

namespace {

std::wstring WideAsciiLower(std::wstring_view s)
{
  std::wstring out(s);
  for (auto& ch : out) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
  }
  return out;
}

std::optional<std::uint64_t> ParseHexOffset(std::wstring_view s)
{
  if (s.size() >= 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
    s.remove_prefix(2);
  }
  if (s.empty() || s.size() > 16) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t ch : s) {
    std::uint64_t digit = 0;
    if (ch >= L'0' && ch <= L'9') {
      digit = static_cast<std::uint64_t>(ch - L'0');
    } else if (ch >= L'a' && ch <= L'f') {
      digit = static_cast<std::uint64_t>(ch - L'a') + 10u;
    } else if (ch >= L'A' && ch <= L'F') {
      digit = static_cast<std::uint64_t>(ch - L'A') + 10u;
    } else {
      return std::nullopt;
    }
    value = (value << 4u) | digit;
  }
  return value;
}

}  // namespace

bool CrashLoggerLocationContradictsDumpFault(
  std::wstring_view location,
  std::wstring_view faultModuleFilename,
  std::uint64_t faultModuleOffset)
{
  if (faultModuleFilename.empty()) {
    return false;
  }
  const std::wstring faultLower = WideAsciiLower(faultModuleFilename);
  if (faultLower == L"crashlogger.dll" || faultLower == L"crashloggersse.dll") {
    return false;
  }
  const auto plus = location.rfind(L'+');
  if (plus == std::wstring_view::npos || plus == 0) {
    return false;
  }
  const auto offset = ParseHexOffset(location.substr(plus + 1));
  if (!offset) {
    return false;
  }
  std::wstring_view module = location.substr(0, plus);
  const auto slash = module.find_last_of(L"/\\");
  if (slash != std::wstring_view::npos) {
    module.remove_prefix(slash + 1);
  }
  if (module.empty()) {
    return false;
  }
  return WideAsciiLower(module) != faultLower || *offset != faultModuleOffset;
}

bool IsLocationObjectTypeAscii(std::string_view objType)
{
  const std::string lower = AsciiLower(objType);
  return lower.find("tesobjectcell") != std::string::npos || lower.find("tesworldspace") != std::string::npos;
}

std::vector<CrashLoggerObjectRef> ParseCrashLoggerUnattributedObjectsAscii(std::string_view logUtf8)
{
  std::vector<CrashLoggerObjectRef> out;
  const std::size_t sectionStart = logUtf8.find("POSSIBLE RELEVANT OBJECTS:");
  if (sectionStart == std::string_view::npos) {
    return out;
  }
  std::size_t pos = logUtf8.find('\n', sectionStart);
  while (pos != std::string_view::npos && pos + 1 < logUtf8.size()) {
    const std::size_t begin = pos + 1;
    const std::size_t end = logUtf8.find('\n', begin);
    std::string_view line = logUtf8.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    pos = end;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty() || (line[0] != '\t' && line[0] != ' ')) {
      break;  // end of the section
    }
    const std::string_view trimmed = TrimLeftAscii(line);
    if (trimmed.empty() || StartsWithCaseInsensitiveAscii(trimmed, "Modified by:") || !ExtractEspRefsFromLine(trimmed).empty()) {
      continue;
    }
    const std::string location = ExtractLocation(trimmed);
    if (location.empty()) {
      continue;
    }
    CrashLoggerObjectRef ref;
    ref.location = location;
    ref.object_type = ExtractObjectType(trimmed);
    ref.object_name = ExtractObjectName(trimmed);
    const std::size_t formStart = trimmed.find("[0x");
    if (formStart != std::string_view::npos) {
      const std::size_t formEnd = trimmed.find(']', formStart);
      if (formEnd != std::string_view::npos) {
        ref.form_id = std::string(trimmed.substr(formStart + 1, formEnd - formStart - 1));
      }
    }
    if ((ref.object_name.empty() && ref.form_id.empty()) || IsLocationObjectTypeAscii(ref.object_type)) {
      continue;  // a bare pointer type, or only where the player was
    }
    ref.relevance_score = LocationWeight(location) + TypeWeight(ref.object_type);
    out.push_back(std::move(ref));
  }
  std::stable_sort(out.begin(), out.end(), [](const CrashLoggerObjectRef& a, const CrashLoggerObjectRef& b) {
    return a.relevance_score > b.relevance_score;
  });
  if (out.size() > 6) {
    out.resize(6);
  }
  return out;
}

}  // namespace crashlogger_core
}  // namespace skydiag::dump_tool
