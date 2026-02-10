#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace skydiag::dump_tool::internal::stackwalk_internal {
namespace {

using skydiag::dump_tool::minidump::ModuleInfo;

std::wstring ReadEnvVar(const wchar_t* name)
{
  if (!name || !*name) {
    return {};
  }
  const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
  if (need == 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(need - 1), L'\0');
  if (!out.empty()) {
    GetEnvironmentVariableW(name, out.data(), need);
  }
  return out;
}

std::wstring ResolveDefaultSymbolCacheDir()
{
  std::wstring fromEnv = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_CACHE_DIR");
  if (!fromEnv.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(fromEnv, ec);
    return fromEnv;
  }

  std::filesystem::path cacheDir;
  if (const std::wstring localAppData = ReadEnvVar(L"LOCALAPPDATA"); !localAppData.empty()) {
    cacheDir = std::filesystem::path(localAppData) / L"SkyrimDiag" / L"SymbolCache";
  } else {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
      cacheDir = std::filesystem::path(tmp) / L"SkyrimDiagSymbolCache";
    }
  }

  if (!cacheDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
  }
  return cacheDir.wstring();
}

std::wstring ResolveSymbolSearchPath(std::wstring* outCachePath, bool allowOnlineSymbols)
{
  if (outCachePath) {
    outCachePath->clear();
  }

  if (const std::wstring explicitPath = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_PATH"); !explicitPath.empty()) {
    return explicitPath;
  }

  if (const std::wstring ntSymbolPath = ReadEnvVar(L"_NT_SYMBOL_PATH"); !ntSymbolPath.empty()) {
    return ntSymbolPath;
  }

  const std::wstring cachePath = ResolveDefaultSymbolCacheDir();
  if (outCachePath) {
    *outCachePath = cachePath;
  }
  if (cachePath.empty()) {
    return {};
  }
  if (!allowOnlineSymbols) {
    return cachePath;
  }
  return L"srv*" + cachePath + L"*https://msdl.microsoft.com/download/symbols";
}

}  // namespace

SymSession::SymSession(const std::vector<ModuleInfo>& modules, bool allowOnlineSymbols)
{
  process = GetCurrentProcess();

  DWORD opts = SymGetOptions();
  opts |= SYMOPT_UNDNAME;
  opts |= SYMOPT_DEFERRED_LOADS;
  opts |= SYMOPT_FAIL_CRITICAL_ERRORS;
  opts |= SYMOPT_NO_PROMPTS;
  SymSetOptions(opts);

  searchPath = ResolveSymbolSearchPath(&cachePath, allowOnlineSymbols);
  ok = SymInitializeW(process, searchPath.empty() ? nullptr : searchPath.c_str(), FALSE) ? true : false;
  if (!ok) {
    return;
  }

  wchar_t actualSearchPath[4096]{};
  if (SymGetSearchPathW(process, actualSearchPath, static_cast<DWORD>(std::size(actualSearchPath))) &&
      actualSearchPath[0] != L'\0') {
    searchPath = actualSearchPath;
  }
  usedOnlineSymbolSource = (searchPath.find(L"https://") != std::wstring::npos);

  for (const auto& m : modules) {
    if (m.path.empty() || m.base == 0 || m.end <= m.base) {
      continue;
    }
    const DWORD64 base = static_cast<DWORD64>(m.base);
    const DWORD size = static_cast<DWORD>(std::min<std::uint64_t>(m.end - m.base, 0xFFFFFFFFull));
    SymLoadModuleExW(process, nullptr, m.path.c_str(), m.filename.empty() ? nullptr : m.filename.c_str(), base, size, nullptr, 0);
  }
}

SymSession::~SymSession()
{
  if (ok && process) {
    SymCleanup(process);
  }
}

}  // namespace skydiag::dump_tool::internal::stackwalk_internal

