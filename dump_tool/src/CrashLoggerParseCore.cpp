#include "CrashLoggerParseCore.h"

#include <algorithm>
#include <cctype>
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
  std::istringstream iss{ std::string(iniUtf8) };
  std::string line;

  bool inDebug = false;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    std::string_view s = TrimAscii(line);
    if (s.empty()) {
      continue;
    }
    const char c0 = s.front();
    if (c0 == ';' || c0 == '#') {
      continue;
    }

    if (c0 == '[' && s.back() == ']') {
      const std::string_view sec = TrimAscii(s.substr(1, s.size() - 2));
      inDebug = EqualsCaseInsensitiveAscii(sec, "debug");
      continue;
    }

    if (!inDebug) {
      continue;
    }

    const auto eq = s.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = TrimRightAscii(s.substr(0, eq));
    std::string_view val = TrimAscii(s.substr(eq + 1));

    if (!EqualsCaseInsensitiveAscii(key, "crashlog directory")) {
      continue;
    }

    // Strip quotes and/or inline comments.
    if (!val.empty() && val.front() == '"') {
      const auto endq = val.find('"', 1);
      if (endq != std::string_view::npos) {
        val = val.substr(1, endq - 1);
      } else {
        val.remove_prefix(1);
      }
      val = TrimAscii(val);
    } else {
      const auto cut = val.find_first_of(";#");
      if (cut != std::string_view::npos) {
        val = TrimRightAscii(val.substr(0, cut));
      }
    }

    if (val.empty()) {
      return std::nullopt;
    }
    return std::string(val);
  }

  return std::nullopt;
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
         ContainsCaseInsensitiveAscii(utf8Prefix, "thread dump") ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "probable call stack") ||
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

std::vector<std::string> ParseCrashLoggerTopModulesAsciiLower(std::string_view logUtf8)
{
  std::unordered_map<std::string, std::uint32_t> freq;
  const bool isThreadDump = ContainsCaseInsensitiveAscii(logUtf8, "thread dump");

  std::istringstream iss{ std::string(logUtf8) };
  std::string line;

  bool inStack = false;
  bool inThreadCallstack = false;

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (isThreadDump) {
      if (!inThreadCallstack) {
        if (ContainsCaseInsensitiveAscii(line, "callstack:")) {
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

      auto tokOpt = TryExtractModulePlusOffsetTokenAscii(line);
      if (!tokOpt) {
        continue;
      }
      const std::string tokLower = AsciiLower(*tokOpt);
      const auto plus = tokLower.find('+');
      if (plus == std::string::npos || plus == 0) {
        continue;
      }
      const std::string module = tokLower.substr(0, plus);
      freq[module] += 1;
      continue;
    }

    if (!inStack) {
      if (ContainsCaseInsensitiveAscii(line, "probable call stack")) {
        inStack = true;
      }
      continue;
    }

    if (line.empty() || ContainsCaseInsensitiveAscii(line, "registers:") || ContainsCaseInsensitiveAscii(line, "modules:")) {
      break;
    }

    auto tokOpt = TryExtractModulePlusOffsetTokenAscii(line);
    if (!tokOpt) {
      continue;
    }
    const std::string tokLower = AsciiLower(*tokOpt);
    const auto plus = tokLower.find('+');
    if (plus == std::string::npos || plus == 0) {
      continue;
    }

    const std::string module = tokLower.substr(0, plus);
    freq[module] += 1;
  }

  struct Row
  {
    std::string moduleLower;
    std::uint32_t count = 0;
  };

  std::vector<Row> rows;
  rows.reserve(freq.size());
  for (const auto& [k, v] : freq) {
    rows.push_back(Row{ k, v });
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.count != b.count) {
      return a.count > b.count;
    }
    return a.moduleLower < b.moduleLower;
  });

  std::vector<std::string> out;
  out.reserve(std::min<std::size_t>(rows.size(), 8));
  for (const auto& r : rows) {
    if (IsSystemishModuleAsciiLower(r.moduleLower) || IsGameExeModuleAsciiLower(r.moduleLower)) {
      continue;
    }
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
      if (LooksLikePluginExtension(name) && name.find(' ') == std::string_view::npos) {
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
        if (LooksLikePluginExtension(name) && name.find(' ') == std::string_view::npos) {
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
  return {};
}

std::string ExtractObjectName(std::string_view line)
{
  auto typeEnd = line.find("*) ");
  if (typeEnd == std::string_view::npos) return {};
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

std::vector<CrashLoggerObjectRef> ParseCrashLoggerObjectRefsAscii(std::string_view logUtf8)
{
  std::vector<CrashLoggerObjectRef> results;
  if (logUtf8.empty()) return results;

  std::istringstream iss{std::string(logUtf8)};
  std::string line;

  enum class Section { kNone, kPossibleObjects, kRegisters };
  Section section = Section::kNone;
  std::string currentRegister;

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

      bool isSubLine = (!line.empty() && (line[0] == '\t' || line[0] == ' '));

      if (!isSubLine) {
        std::string location = ExtractLocation(trimmed);
        if (!location.empty()) {
          currentRegister = location;
          auto espRefs = ExtractEspRefsFromLine(trimmed);
          std::string objType = ExtractObjectType(trimmed);
          std::string objName = ExtractObjectName(trimmed);
          for (const auto& entry : espRefs) {
            const std::string lower = AsciiLower(entry.esp_name);
            if (IsVanillaDlcEspAsciiLower(lower)) continue;
            CrashLoggerObjectRef ref;
            ref.location = currentRegister;
            ref.object_type = objType;
            ref.esp_name = entry.esp_name;
            ref.object_name = objName;
            ref.form_id = entry.form_id;
            ref.relevance_score = LocationWeight(currentRegister) + TypeWeight(objType);
            results.push_back(std::move(ref));
          }
        }
      } else {
        if (!currentRegister.empty()) {
          if (StartsWithCaseInsensitiveAscii(trimmed, "Modified by:") ||
              StartsWithCaseInsensitiveAscii(trimmed, "modified by:")) {
            continue;
          }
          auto espRefs = ExtractEspRefsFromLine(trimmed);
          for (const auto& entry : espRefs) {
            const std::string lower = AsciiLower(entry.esp_name);
            if (IsVanillaDlcEspAsciiLower(lower)) continue;
            CrashLoggerObjectRef ref;
            ref.location = currentRegister;
            ref.object_type = "";
            ref.esp_name = entry.esp_name;
            ref.form_id = entry.form_id;
            ref.relevance_score = LocationWeight(currentRegister) + TypeWeight("");
            results.push_back(std::move(ref));
          }
        }
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

}  // namespace crashlogger_core
}  // namespace skydiag::dump_tool
