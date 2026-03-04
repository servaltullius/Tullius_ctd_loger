#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool {
namespace crashlogger_core {

struct CrashLoggerCppExceptionDetails {
  std::string type;
  std::string info;
  std::string throw_location;
  std::string module;
};

inline std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

inline std::optional<std::string> ParseCrashLoggerVersionAscii(std::string_view logUtf8)
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

inline std::string_view TrimLeftAscii(std::string_view s)
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

inline std::string_view TrimRightAscii(std::string_view s)
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

inline std::string_view TrimAscii(std::string_view s)
{
  return TrimRightAscii(TrimLeftAscii(s));
}

inline bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle)
{
  if (needle.empty()) {
    return true;
  }
  const std::string h = AsciiLower(haystack);
  const std::string n = AsciiLower(needle);
  return h.find(n) != std::string::npos;
}

inline bool StartsWithCaseInsensitiveAscii(std::string_view s, std::string_view prefix)
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

inline bool EqualsCaseInsensitiveAscii(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  return StartsWithCaseInsensitiveAscii(a, b);
}

inline std::optional<std::string> ParseCrashLoggerIniCrashlogDirectoryAscii(std::string_view iniUtf8)
{
  // CrashLogger.ini uses [Debug] Crashlog Directory=... to override output directory.
  // We parse this ourselves (best-effort) to improve offline log discovery when users customize the location.
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
      // Handle: "C:\path with spaces" ; comment
      const auto endq = val.find('"', 1);
      if (endq != std::string_view::npos) {
        val = val.substr(1, endq - 1);
      } else {
        val.remove_prefix(1);
      }
      val = TrimAscii(val);
    } else {
      // Handle: C:\path ; comment
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

inline bool IsSystemishModuleAsciiLower(std::string_view filenameLower)
{
  const char* k[] = {
    "kernelbase.dll", "ntdll.dll",       "kernel32.dll",      "ucrtbase.dll",
    "msvcp140.dll",   "vcruntime140.dll","vcruntime140_1.dll","concrt140.dll",
    "user32.dll",     "gdi32.dll",       "combase.dll",       "ole32.dll",
    "ws2_32.dll",     "win32u.dll",
  };
  for (const auto* m : k) {
    if (filenameLower == m) {
      return true;
    }
  }
  return false;
}

inline bool IsGameExeModuleAsciiLower(std::string_view filenameLower)
{
  return filenameLower == "skyrimse.exe" || filenameLower == "skyrimae.exe" ||
         filenameLower == "skyrimvr.exe" || filenameLower == "skyrim.exe";
}

inline bool LooksLikeCrashLoggerLogTextCore(std::string_view utf8Prefix)
{
  // Heuristic: CrashLogger emits "CrashLoggerSSE ..." and either "CRASH TIME:" (crash logs)
  // or "THREAD DUMP" (manual hang diagnosis).
  if (!ContainsCaseInsensitiveAscii(utf8Prefix, "crashlogger")) {
    return false;
  }
  return ContainsCaseInsensitiveAscii(utf8Prefix, "crash time:") ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "thread dump") ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "probable call stack") ||
         ContainsCaseInsensitiveAscii(utf8Prefix, "process info:");
}

inline std::optional<std::string_view> TryExtractModulePlusOffsetTokenAscii(std::string_view line)
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

inline std::optional<CrashLoggerCppExceptionDetails> ParseCrashLoggerCppExceptionDetailsAscii(std::string_view logUtf8)
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

inline std::vector<std::string> ParseCrashLoggerTopModulesAsciiLower(std::string_view logUtf8)
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

// Parsed date/time components from a filename pattern.
struct ParsedTimestamp
{
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// Extract YYYYMMDD_HHMMSS from a filename stem. Returns the first valid match found.
inline std::optional<ParsedTimestamp> TryExtractCompactTimestampFromStem(std::wstring_view stem)
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
inline std::optional<ParsedTimestamp> TryExtractDashedTimestampFromStem(std::wstring_view stem)
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

}  // namespace crashlogger_core

using crashlogger_core::LooksLikeCrashLoggerLogTextCore;
using crashlogger_core::ParseCrashLoggerVersionAscii;
using crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii;
using crashlogger_core::ParseCrashLoggerTopModulesAsciiLower;

}  // namespace skydiag::dump_tool
