#include "NativeApi.h"

#include "Analyzer.h"
#include "DumpToolStartupUtil.h"
#include "OutputWriter.h"
#include "Utf.h"

#include <cwchar>
#include <exception>
#include <string>

namespace {

using namespace skydiag::dump_tool::startup;

void SetError(std::wstring_view msg, wchar_t* buf, int cap)
{
  if (!buf || cap <= 0) {
    return;
  }
  buf[0] = L'\0';
  if (msg.empty()) {
    return;
  }
  _snwprintf_s(buf, static_cast<size_t>(cap), _TRUNCATE, L"%ls", msg.data());
}

std::wstring DescribeStdException(const std::exception& ex)
{
  const char* what = ex.what();
  if (!what || !*what) {
    return L"(std::exception with empty what())";
  }
  // Best-effort: most of our dependencies format exceptions as UTF-8.
  const std::wstring w = skydiag::dump_tool::Utf8ToWide(std::string_view(what));
  return w.empty() ? L"(std::exception message decode failed)" : w;
}

}  // namespace

int __stdcall SkyrimDiagAnalyzeDumpW(
  const wchar_t* dumpPath,
  const wchar_t* outDir,
  const wchar_t* languageToken,
  int debug,
  wchar_t* errorBuf,
  int errorBufChars)
{
  using namespace skydiag::dump_tool;

  if (!dumpPath || dumpPath[0] == L'\0') {
    SetError(L"dumpPath is empty", errorBuf, errorBufChars);
    return 2;
  }

  AnalyzeOptions opt{};
  opt.debug = (debug != 0);
  opt.allow_online_symbols = ReadEnvBool(L"SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS", false);
  opt.redact_paths = !opt.debug;
  if (languageToken && languageToken[0] != L'\0') {
    const std::string ascii = ToAscii(languageToken);
    opt.language = i18n::ParseLanguageTokenAscii(ascii);
  }
  const std::wstring exeDir = GetCurrentExeDir();
  if (!exeDir.empty()) {
    opt.data_dir = (std::filesystem::path(exeDir) / L"data").wstring();
  }
  if (outDir && outDir[0] != L'\0') {
    opt.output_dir = outDir;
  }
  const std::wstring gameVersionW = ReadEnvString(L"SKYRIMDIAG_GAME_VERSION");
  if (!gameVersionW.empty()) {
    opt.game_version = ToAscii(gameVersionW);
  }

  AnalysisResult result{};
  std::wstring err;

  const std::wstring dumpPathW(dumpPath);
  const std::wstring outDirW = (outDir ? std::wstring(outDir) : std::wstring{});

  try {
    if (!AnalyzeDump(dumpPathW, outDirW, opt, result, &err)) {
      SetError(err, errorBuf, errorBufChars);
      return 3;
    }
  } catch (const std::exception& ex) {
    const std::wstring msg = L"Native exception during AnalyzeDump: " + DescribeStdException(ex);
    SetError(msg, errorBuf, errorBufChars);
    return 3;
  } catch (...) {
    SetError(L"Native exception during AnalyzeDump (non-std exception)", errorBuf, errorBufChars);
    return 3;
  }

  try {
    if (!WriteOutputs(result, &err)) {
      SetError(err, errorBuf, errorBufChars);
      return 4;
    }
  } catch (const std::exception& ex) {
    const std::wstring msg = L"Native exception during WriteOutputs: " + DescribeStdException(ex);
    SetError(msg, errorBuf, errorBufChars);
    return 4;
  } catch (...) {
    SetError(L"Native exception during WriteOutputs (non-std exception)", errorBuf, errorBufChars);
    return 4;
  }

  SetError(L"", errorBuf, errorBufChars);
  return 0;
}
