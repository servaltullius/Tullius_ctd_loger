#include <Windows.h>

#include "Analyzer.h"
#include "DumpToolCliArgs.h"
#include "I18nCore.h"
#include "OutputWriter.h"

#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string ToAscii(std::wstring_view w)
{
  std::string out;
  out.reserve(w.size());
  for (const wchar_t c : w) {
    if (c >= 0 && c <= 0x7f) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

bool ParseBoolText(std::wstring_view text, bool defaultValue)
{
  if (text.empty()) {
    return defaultValue;
  }
  std::wstring lower;
  lower.reserve(text.size());
  for (const wchar_t ch : text) {
    lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
  }
  if (lower == L"1" || lower == L"true" || lower == L"yes" || lower == L"on") {
    return true;
  }
  if (lower == L"0" || lower == L"false" || lower == L"no" || lower == L"off") {
    return false;
  }
  return defaultValue;
}

bool ReadEnvBool(const wchar_t* key, bool defaultValue)
{
  if (!key || !*key) {
    return defaultValue;
  }
  const DWORD need = GetEnvironmentVariableW(key, nullptr, 0);
  if (need == 0) {
    return defaultValue;
  }
  std::wstring value(static_cast<std::size_t>(need - 1), L'\0');
  if (!value.empty()) {
    GetEnvironmentVariableW(key, value.data(), need);
  }
  return ParseBoolText(value, defaultValue);
}

std::wstring ReadEnvString(const wchar_t* key)
{
  if (!key || !*key) {
    return {};
  }
  const DWORD need = GetEnvironmentVariableW(key, nullptr, 0);
  if (need == 0) {
    return {};
  }
  std::wstring value(static_cast<std::size_t>(need - 1), L'\0');
  if (!value.empty()) {
    GetEnvironmentVariableW(key, value.data(), need);
  }
  return value;
}

std::wstring GetCurrentExeDir()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring_view(buf.data(), n)).parent_path().wstring();
}

void BestEffortLowerPriority()
{
  // Headless analysis is post-incident; prefer to be a "good citizen" if other
  // apps are still active.
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  using namespace skydiag::dump_tool;

  BestEffortLowerPriority();

  std::vector<std::wstring_view> args;
  if (argc > 0 && argv) {
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; i++) {
      const wchar_t* s = argv[i];
      args.emplace_back(s ? std::wstring_view(s) : std::wstring_view{});
    }
  }

  cli::DumpToolCliArgs a{};
  std::wstring parseErr;
  if (!cli::ParseDumpToolCliArgs(args, &a, &parseErr)) {
    if (!parseErr.empty() && parseErr.rfind(L"Usage:", 0) == 0) {
      std::wcout << parseErr;
      return 0;
    }
    if (!parseErr.empty()) {
      std::wcerr << L"[SkyrimDiagDumpToolCli] " << parseErr << L"\n";
    }
    std::wcerr << cli::DumpToolCliUsage();
    return 2;
  }

  AnalyzeOptions opt{};
  opt.debug = a.debug;
  opt.redact_paths = !a.debug;
  if (a.allow_online_symbols.has_value()) {
    opt.allow_online_symbols = a.allow_online_symbols.value();
  } else {
    opt.allow_online_symbols = ReadEnvBool(L"SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS", false);
  }
  if (!a.lang_token.empty()) {
    opt.language = i18n::ParseLanguageTokenAscii(ToAscii(a.lang_token));
  }
  const std::wstring exeDir = GetCurrentExeDir();
  if (!exeDir.empty()) {
    opt.data_dir = (std::filesystem::path(exeDir) / L"data").wstring();
  }
  if (!a.out_dir.empty()) {
    opt.output_dir = a.out_dir;
  }
  const std::wstring gameVersionW = ReadEnvString(L"SKYRIMDIAG_GAME_VERSION");
  if (!gameVersionW.empty()) {
    opt.game_version = ToAscii(gameVersionW);
  }

  AnalysisResult result{};
  std::wstring err;
  if (!AnalyzeDump(a.dump_path, a.out_dir, opt, result, &err)) {
    std::wcerr << L"[SkyrimDiagDumpToolCli] AnalyzeDump failed: " << err << L"\n";
    return 3;
  }
  if (!WriteOutputs(result, &err)) {
    std::wcerr << L"[SkyrimDiagDumpToolCli] WriteOutputs failed: " << err << L"\n";
    return 4;
  }

  return 0;
}
