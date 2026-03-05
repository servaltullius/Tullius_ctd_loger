#include <Windows.h>

#include "Analyzer.h"
#include "DumpToolCliArgs.h"
#include "DumpToolStartupUtil.h"
#include "I18nCore.h"
#include "OutputWriter.h"

#include <iostream>

namespace {

using namespace skydiag::dump_tool::startup;

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
