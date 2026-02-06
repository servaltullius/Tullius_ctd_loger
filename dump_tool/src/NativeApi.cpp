#include "NativeApi.h"

#include "Analyzer.h"
#include "OutputWriter.h"

#include <cwchar>
#include <string>

namespace {

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

std::string ToAscii(std::wstring_view w)
{
  std::string out;
  out.reserve(w.size());
  for (wchar_t c : w) {
    if (c >= 0 && c <= 0x7f) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
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
  if (languageToken && languageToken[0] != L'\0') {
    const std::string ascii = ToAscii(languageToken);
    opt.language = i18n::ParseLanguageTokenAscii(ascii);
  }

  AnalysisResult result{};
  std::wstring err;

  const std::wstring dumpPathW(dumpPath);
  const std::wstring outDirW = (outDir ? std::wstring(outDir) : std::wstring{});

  if (!AnalyzeDump(dumpPathW, outDirW, opt, result, &err)) {
    SetError(err, errorBuf, errorBufChars);
    return 3;
  }

  if (!WriteOutputs(result, &err)) {
    SetError(err, errorBuf, errorBufChars);
    return 4;
  }

  SetError(L"", errorBuf, errorBufChars);
  return 0;
}

