#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <system_error>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk_internal {
namespace {

using skydiag::dump_tool::minidump::ModuleInfo;

std::mutex& DbgHelpGlobalMutex()
{
  static std::mutex m;
  return m;
}

std::string QueryFileVersionString(const std::filesystem::path& filePath)
{
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(filePath.c_str(), &handle);
  if (size == 0) {
    return {};
  }

  std::vector<std::uint8_t> buffer(size);
  if (!GetFileVersionInfoW(filePath.c_str(), handle, size, buffer.data())) {
    return {};
  }

  VS_FIXEDFILEINFO* ffi = nullptr;
  UINT ffiSize = 0;
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiSize) ||
      !ffi || ffiSize < sizeof(VS_FIXEDFILEINFO)) {
    return {};
  }

  const std::uint32_t ms = ffi->dwFileVersionMS;
  const std::uint32_t ls = ffi->dwFileVersionLS;
  return std::to_string(HIWORD(ms)) + "."
       + std::to_string(LOWORD(ms)) + "."
       + std::to_string(HIWORD(ls)) + "."
       + std::to_string(LOWORD(ls));
}

std::wstring QueryLoadedModulePath(const wchar_t* moduleName)
{
  if (!moduleName || !*moduleName) {
    return {};
  }
  const HMODULE module = GetModuleHandleW(moduleName);
  if (!module) {
    return {};
  }

  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD copied = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size() - 1) {
      buffer.resize(copied);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring SearchDllPath(const wchar_t* moduleName)
{
  if (!moduleName || !*moduleName) {
    return {};
  }
  const DWORD needed = SearchPathW(nullptr, moduleName, nullptr, 0, nullptr, nullptr);
  if (needed == 0) {
    return {};
  }
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  if (SearchPathW(nullptr, moduleName, nullptr, needed, buffer.data(), nullptr) == 0) {
    return {};
  }
  buffer.resize(std::wcslen(buffer.c_str()));
  return buffer;
}

std::wstring ResolveRuntimeDllPath(const wchar_t* moduleName)
{
  if (const std::wstring loaded = QueryLoadedModulePath(moduleName); !loaded.empty()) {
    return loaded;
  }
  return SearchDllPath(moduleName);
}

std::filesystem::path InferGameExeDirFromModules(const std::vector<ModuleInfo>& modules)
{
  for (const auto& module : modules) {
    if (module.path.empty() || module.filename.empty() || !minidump::IsGameExeModule(module.filename)) {
      continue;
    }
    const std::filesystem::path exePath(module.path);
    if (exePath.has_parent_path()) {
      return exePath.parent_path();
    }
  }
  return {};
}

std::wstring FindBundledGameRuntimeDllPath(const std::vector<ModuleInfo>& modules, const wchar_t* moduleName)
{
  if (!moduleName || !*moduleName) {
    return {};
  }

  const std::filesystem::path gameExeDir = InferGameExeDirFromModules(modules);
  if (gameExeDir.empty()) {
    return {};
  }

  const auto candidate = gameExeDir / L"Data" / L"SKSE" / L"Plugins" / moduleName;
  return std::filesystem::exists(candidate) ? candidate.wstring() : std::wstring{};
}

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

bool WideContainsAsciiInsensitive(std::wstring_view haystack, std::string_view needleAscii)
{
  if (haystack.empty() || needleAscii.empty()) {
    return false;
  }

  std::wstring needle;
  needle.reserve(needleAscii.size());
  for (char ch : needleAscii) {
    needle.push_back(static_cast<wchar_t>(std::towlower(static_cast<wchar_t>(static_cast<unsigned char>(ch)))));
  }

  std::wstring lowered(haystack);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return lowered.find(needle) != std::wstring::npos;
}

std::wstring ResolveDefaultSymbolCacheDir(bool* outReady)
{
  if (outReady) {
    *outReady = false;
  }
  std::wstring fromEnv = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_CACHE_DIR");
  if (!fromEnv.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(fromEnv, ec);
    if (outReady) {
      *outReady = !ec && std::filesystem::exists(fromEnv);
    }
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
    if (outReady) {
      *outReady = !ec && std::filesystem::exists(cacheDir);
    }
  }
  return cacheDir.wstring();
}

std::wstring ResolveSymbolSearchPath(
  std::wstring* outCachePath,
  bool allowOnlineSymbols,
  bool* outCacheReady,
  bool* outFromEnv)
{
  if (outCachePath) {
    outCachePath->clear();
  }
  if (outCacheReady) {
    *outCacheReady = false;
  }
  if (outFromEnv) {
    *outFromEnv = false;
  }

  if (const std::wstring explicitPath = ReadEnvVar(L"SKYRIMDIAG_SYMBOL_PATH"); !explicitPath.empty()) {
    if (outFromEnv) {
      *outFromEnv = true;
    }
    return explicitPath;
  }

  if (const std::wstring ntSymbolPath = ReadEnvVar(L"_NT_SYMBOL_PATH"); !ntSymbolPath.empty()) {
    if (outFromEnv) {
      *outFromEnv = true;
    }
    return ntSymbolPath;
  }

  const std::wstring cachePath = ResolveDefaultSymbolCacheDir(outCacheReady);
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
  dbghelp_lock = std::unique_lock<std::mutex>(DbgHelpGlobalMutex());
  process = GetCurrentProcess();

  dbghelpPath = ResolveRuntimeDllPath(L"dbghelp.dll");
  if (!dbghelpPath.empty()) {
    const auto version = QueryFileVersionString(std::filesystem::path(dbghelpPath));
    dbghelpVersion.assign(version.begin(), version.end());
  }
  if (dbghelpPath.empty()) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] dbghelp.dll runtime not resolved; stackwalk quality may be degraded");
  } else if (dbghelpVersion.empty()) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] dbghelp.dll version unreadable; runtime health is uncertain");
  }

  msdiaPath = ResolveRuntimeDllPath(L"msdia140.dll");
  if (msdiaPath.empty()) {
    if (const std::wstring bundledMsdiaPath = FindBundledGameRuntimeDllPath(modules, L"msdia140.dll");
        !bundledMsdiaPath.empty()) {
      ownedMsdiaModule = LoadLibraryW(bundledMsdiaPath.c_str());
      if (ownedMsdiaModule) {
        msdiaPath = QueryLoadedModulePath(L"msdia140.dll");
        if (msdiaPath.empty()) {
          msdiaPath = bundledMsdiaPath;
        }
      }
    }
  }
  msdiaAvailable = !msdiaPath.empty();
  if (!msdiaAvailable) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] msdia140.dll not found; source line resolution may be limited");
  }

  DWORD opts = SymGetOptions();
  opts |= SYMOPT_UNDNAME;
  opts |= SYMOPT_DEFERRED_LOADS;
  opts |= SYMOPT_FAIL_CRITICAL_ERRORS;
  opts |= SYMOPT_NO_PROMPTS;
  SymSetOptions(opts);

  bool searchPathFromEnv = false;
  searchPath = ResolveSymbolSearchPath(&cachePath, allowOnlineSymbols, &symbolCacheReady, &searchPathFromEnv);
  if (searchPath.empty()) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] symbol search path is empty; symbolization will be limited");
  }
  if (!cachePath.empty() && !symbolCacheReady) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] symbol cache directory unavailable; local-cache symbolization may be limited");
  }
  if (!allowOnlineSymbols &&
      searchPathFromEnv &&
      (WideContainsAsciiInsensitive(searchPath, "https://") || WideContainsAsciiInsensitive(searchPath, "http://"))) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] explicit symbol path includes online source while policy disables it");
  }
  ok = SymInitializeW(process, searchPath.empty() ? nullptr : searchPath.c_str(), FALSE) ? true : false;
  if (!ok) {
    runtimeDegraded = true;
    runtimeDiagnostics.push_back(L"[Symbols] SymInitializeW failed; stackwalk symbolization unavailable");
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
  if (ownedMsdiaModule) {
    FreeLibrary(ownedMsdiaModule);
  }
}

}  // namespace skydiag::dump_tool::internal::stackwalk_internal
