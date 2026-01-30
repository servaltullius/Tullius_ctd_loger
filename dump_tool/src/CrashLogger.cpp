#include "CrashLogger.h"

#include <Windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool {
namespace {

std::wstring ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }

  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }

  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return out;
}

std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
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

bool IsSystemishModule(std::wstring_view filename)
{
  std::wstring lower(filename);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

  const wchar_t* k[] = {
    L"kernelbase.dll", L"ntdll.dll",     L"kernel32.dll",  L"ucrtbase.dll",
    L"msvcp140.dll",   L"vcruntime140.dll", L"vcruntime140_1.dll", L"concrt140.dll", L"user32.dll",
    L"gdi32.dll",      L"combase.dll",   L"ole32.dll",     L"ws2_32.dll",
  };
  for (const auto* m : k) {
    if (lower == m) {
      return true;
    }
  }
  return false;
}

bool IsGameExeModule(std::wstring_view filename)
{
  const std::wstring lower = WideLower(filename);
  return (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe");
}

std::optional<std::wstring> TryGetDocumentsKnownFolder()
{
  PWSTR path = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path);
  if (FAILED(hr) || !path) {
    return std::nullopt;
  }
  std::wstring out(path);
  CoTaskMemFree(path);
  return out;
}

std::uint64_t FileTimeToU64(const FILETIME& ft)
{
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<std::uint64_t>(u.QuadPart);
}

std::uint64_t AbsDiffU64(std::uint64_t a, std::uint64_t b)
{
  return (a > b) ? (a - b) : (b - a);
}

bool TryGetFileLastWriteTimeU64(const std::wstring& path, std::uint64_t& out, std::wstring* err)
{
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    if (err) *err = L"GetFileAttributesExW failed: " + std::to_wstring(GetLastError());
    return false;
  }
  out = FileTimeToU64(fad.ftLastWriteTime);
  if (err) err->clear();
  return true;
}

std::optional<std::uint64_t> TryParseLocalTimestampInFilenameToFileTimeUtc(std::wstring_view stem)
{
  // Search for pattern: YYYYMMDD_HHMMSS (local time)
  const auto is_digits = [](std::wstring_view s) {
    for (wchar_t c : s) {
      if (c < L'0' || c > L'9') {
        return false;
      }
    }
    return true;
  };

  std::optional<std::size_t> bestPos;
  for (std::size_t i = 0; i + 15 <= stem.size(); i++) {
    const std::wstring_view date = stem.substr(i, 8);
    if (!is_digits(date)) {
      continue;
    }
    if (stem[i + 8] != L'_') {
      continue;
    }
    const std::wstring_view time = stem.substr(i + 9, 6);
    if (!is_digits(time)) {
      continue;
    }
    bestPos = i;
  }
  if (!bestPos) {
    return std::nullopt;
  }

  const std::size_t pos = *bestPos;
  const std::wstring_view date = stem.substr(pos, 8);
  const std::wstring_view time = stem.substr(pos + 9, 6);

  const int y = std::stoi(std::wstring(date.substr(0, 4)));
  const int mo = std::stoi(std::wstring(date.substr(4, 2)));
  const int d = std::stoi(std::wstring(date.substr(6, 2)));
  const int hh = std::stoi(std::wstring(time.substr(0, 2)));
  const int mm = std::stoi(std::wstring(time.substr(2, 2)));
  const int ss = std::stoi(std::wstring(time.substr(4, 2)));

  SYSTEMTIME local{};
  local.wYear = static_cast<WORD>(y);
  local.wMonth = static_cast<WORD>(mo);
  local.wDay = static_cast<WORD>(d);
  local.wHour = static_cast<WORD>(hh);
  local.wMinute = static_cast<WORD>(mm);
  local.wSecond = static_cast<WORD>(ss);

  SYSTEMTIME utc{};
  if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
    return std::nullopt;
  }

  FILETIME ft{};
  if (!SystemTimeToFileTime(&utc, &ft)) {
    return std::nullopt;
  }
  return FileTimeToU64(ft);
}

std::optional<std::uint64_t> TryParseCrashLoggerTimestampInFilenameToFileTimeUtc(std::wstring_view stem)
{
  // Search for pattern: YYYY-MM-DD-HH-MM-SS (local time)
  const auto is_digits = [](std::wstring_view s) {
    for (wchar_t c : s) {
      if (c < L'0' || c > L'9') {
        return false;
      }
    }
    return true;
  };

  for (std::size_t i = 0; i + 19 <= stem.size(); i++) {
    const std::wstring_view v = stem.substr(i, 19);
    if (v[4] != L'-' || v[7] != L'-' || v[10] != L'-' || v[13] != L'-' || v[16] != L'-') {
      continue;
    }

    const std::wstring_view yS = v.substr(0, 4);
    const std::wstring_view moS = v.substr(5, 2);
    const std::wstring_view dS = v.substr(8, 2);
    const std::wstring_view hhS = v.substr(11, 2);
    const std::wstring_view mmS = v.substr(14, 2);
    const std::wstring_view ssS = v.substr(17, 2);
    if (!is_digits(yS) || !is_digits(moS) || !is_digits(dS) || !is_digits(hhS) || !is_digits(mmS) || !is_digits(ssS)) {
      continue;
    }

    const int y = std::stoi(std::wstring(yS));
    const int mo = std::stoi(std::wstring(moS));
    const int d = std::stoi(std::wstring(dS));
    const int hh = std::stoi(std::wstring(hhS));
    const int mm = std::stoi(std::wstring(mmS));
    const int ss = std::stoi(std::wstring(ssS));

    if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
      continue;
    }

    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(y);
    local.wMonth = static_cast<WORD>(mo);
    local.wDay = static_cast<WORD>(d);
    local.wHour = static_cast<WORD>(hh);
    local.wMinute = static_cast<WORD>(mm);
    local.wSecond = static_cast<WORD>(ss);

    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) {
      return std::nullopt;
    }

    FILETIME ft{};
    if (!SystemTimeToFileTime(&utc, &ft)) {
      return std::nullopt;
    }
    return FileTimeToU64(ft);
  }

  return std::nullopt;
}

std::uint64_t BestEffortDumpTimestampFileTimeUtc(const std::filesystem::path& dumpPath)
{
  const std::wstring stem = dumpPath.stem().wstring();
  if (auto ft = TryParseLocalTimestampInFilenameToFileTimeUtc(stem)) {
    return *ft;
  }

  std::uint64_t ft = 0;
  std::wstring ignored;
  if (TryGetFileLastWriteTimeU64(dumpPath.wstring(), ft, &ignored)) {
    return ft;
  }

  // Fall back to "now" (should be rare).
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  return FileTimeToU64(now);
}

std::optional<std::string> ReadFilePrefixUtf8(const std::filesystem::path& path, std::size_t maxBytes, std::wstring* err)
{
  HANDLE file =
    CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) *err = L"CreateFileW failed: " + std::to_wstring(GetLastError());
    return std::nullopt;
  }

  std::string out;
  out.resize(maxBytes);

  DWORD got = 0;
  if (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) {
    const DWORD le = GetLastError();
    CloseHandle(file);
    if (err) *err = L"ReadFile failed: " + std::to_wstring(le);
    return std::nullopt;
  }

  CloseHandle(file);
  out.resize(static_cast<std::size_t>(got));
  if (err) err->clear();
  return out;
}

bool LooksLikeCrashLoggerLogText(std::string_view utf8Prefix)
{
  // Heuristic: both strings should appear.
  return ContainsCaseInsensitiveAscii(utf8Prefix, "crashlogger") &&
         ContainsCaseInsensitiveAscii(utf8Prefix, "probable call stack");
}

std::vector<std::filesystem::path> BuildCrashLoggerCandidateDirs(const std::optional<std::filesystem::path>& mo2BaseDir)
{
  std::vector<std::filesystem::path> dirs;
  std::error_code ec;

  // Documents\My Games\...\SKSE
  if (auto docs = TryGetDocumentsKnownFolder()) {
    const std::filesystem::path docRoot(*docs);
    const std::filesystem::path se = docRoot / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
    const std::filesystem::path vr = docRoot / L"My Games" / L"Skyrim VR" / L"SKSE";

    const std::filesystem::path variants[] = {
      se,
      se / L"CrashLogger",
      se / L"CrashLogs",
      se / L"Crashlogs",
      vr,
      vr / L"CrashLogger",
      vr / L"CrashLogs",
      vr / L"Crashlogs",
    };
    for (const auto& d : variants) {
      if (std::filesystem::is_directory(d, ec)) {
        dirs.push_back(d);
      }
    }
  }

  // MO2 base (optional): <base>\overwrite\... and <base>\profiles\<name>\...
  if (mo2BaseDir) {
    const std::filesystem::path base = *mo2BaseDir;
    const std::filesystem::path overwrite = base / L"overwrite";
    const std::filesystem::path profiles = base / L"profiles";

    const std::filesystem::path owVariants[] = {
      overwrite / L"SKSE",
      overwrite / L"SKSE" / L"CrashLogger",
      overwrite / L"SKSE" / L"CrashLogs",
      overwrite / L"SKSE" / L"Crashlogs",
    };
    for (const auto& d : owVariants) {
      if (std::filesystem::is_directory(d, ec)) {
        dirs.push_back(d);
      }
    }

    if (std::filesystem::is_directory(profiles, ec)) {
      for (const auto& ent : std::filesystem::directory_iterator(profiles, ec)) {
        if (ec) {
          break;
        }
        if (!ent.is_directory(ec)) {
          continue;
        }
        const auto p = ent.path();
        const std::filesystem::path pv[] = {
          p / L"SKSE",
          p / L"SKSE" / L"CrashLogger",
          p / L"SKSE" / L"CrashLogs",
          p / L"SKSE" / L"Crashlogs",
        };
        for (const auto& d : pv) {
          if (std::filesystem::is_directory(d, ec)) {
            dirs.push_back(d);
          }
        }
      }
    }
  }

  return dirs;
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

}  // namespace

std::optional<std::filesystem::path> TryFindCrashLoggerLogForDump(
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& mo2BaseDir,
  std::wstring* err)
{
  const std::uint64_t targetTime = BestEffortDumpTimestampFileTimeUtc(dumpPath);
  const auto dirs = BuildCrashLoggerCandidateDirs(mo2BaseDir);

  // Crash Logger logs should be created around the crash moment. If nothing is close in time, don't attach an older
  // unrelated log (confusing).
  constexpr std::uint64_t kFileTimeTicksPerSec = 10'000'000ull;
  constexpr std::uint64_t kMaxDiffSec = 30ull * 60ull;  // 30 minutes
  constexpr std::uint64_t kMaxDiff = kMaxDiffSec * kFileTimeTicksPerSec;

  std::optional<std::filesystem::path> best;
  std::uint64_t bestDiff = std::numeric_limits<std::uint64_t>::max();

  std::error_code ec;
  for (const auto& dir : dirs) {
    for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!ent.is_regular_file(ec)) {
        continue;
      }

      const auto p = ent.path();
      const std::wstring ext = WideLower(p.extension().wstring());
      if (ext != L".log" && ext != L".txt") {
        continue;
      }

      std::uint64_t ft = 0;
      if (auto nameFt = TryParseCrashLoggerTimestampInFilenameToFileTimeUtc(p.stem().wstring())) {
        ft = *nameFt;
      } else {
        std::wstring timeErr;
        if (!TryGetFileLastWriteTimeU64(p.wstring(), ft, &timeErr)) {
          continue;
        }
      }

      std::wstring prefixErr;
      auto prefix = ReadFilePrefixUtf8(p, 256 * 1024, &prefixErr);
      if (!prefix) {
        continue;
      }
      if (!LooksLikeCrashLoggerLogText(*prefix)) {
        continue;
      }

      const std::uint64_t diff = AbsDiffU64(ft, targetTime);
      if (diff < bestDiff) {
        best = p;
        bestDiff = diff;
      }
    }
  }

  if (!best) {
    if (err) err->clear();
    return std::nullopt;
  }
  if (bestDiff > kMaxDiff) {
    if (err) err->clear();
    return std::nullopt;
  }
  if (err) err->clear();
  return best;
}

std::optional<std::string> ReadWholeFileUtf8(const std::filesystem::path& path, std::wstring* err)
{
  HANDLE file = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) *err = L"CreateFileW failed: " + std::to_wstring(GetLastError());
    return std::nullopt;
  }

  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) {
    const DWORD le = GetLastError();
    CloseHandle(file);
    if (err) *err = L"GetFileSizeEx failed: " + std::to_wstring(le);
    return std::nullopt;
  }

  std::string out;
  out.resize(static_cast<std::size_t>(sz.QuadPart));

  std::size_t off = 0;
  while (off < out.size()) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - off, 1u << 20));
    if (!ReadFile(file, out.data() + off, want, &got, nullptr)) {
      const DWORD le = GetLastError();
      CloseHandle(file);
      if (err) *err = L"ReadFile failed: " + std::to_wstring(le);
      return std::nullopt;
    }
    if (got == 0) {
      break;
    }
    off += got;
  }

  CloseHandle(file);
  out.resize(off);
  if (err) err->clear();
  return out;
}

std::vector<std::wstring> ParseCrashLoggerTopModules(
  std::string_view logUtf8,
  const std::unordered_map<std::wstring, std::wstring>& canonicalByFilenameLower)
{
  std::unordered_map<std::string, std::uint32_t> freq;
  bool inStack = false;

  std::istringstream iss{ std::string(logUtf8) };
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
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
    const std::string_view tok = *tokOpt;
    const auto plus = tok.find('+');
    if (plus == std::string::npos || plus == 0) {
      continue;
    }

    const std::string module = AsciiLower(tok.substr(0, plus));
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
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });

  std::vector<std::wstring> out;
  out.reserve(std::min<std::size_t>(rows.size(), 8));
  for (const auto& r : rows) {
    const std::wstring wLower = ToWide(r.moduleLower);
    const std::wstring key = WideLower(wLower);

    const auto it = canonicalByFilenameLower.find(key);
    std::wstring disp = (it != canonicalByFilenameLower.end()) ? it->second : wLower;

    if (IsSystemishModule(disp) || IsGameExeModule(disp)) {
      continue;
    }

    out.push_back(disp);
    if (out.size() >= 8) {
      break;
    }
  }

  return out;
}

}  // namespace skydiag::dump_tool
