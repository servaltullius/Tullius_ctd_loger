#include "NativeApi.h"

#include "Analyzer.h"
#include "OutputWriter.h"

#include <cwchar>
#include <cwctype>
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
