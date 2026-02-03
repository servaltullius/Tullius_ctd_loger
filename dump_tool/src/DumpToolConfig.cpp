#include "DumpToolConfig.h"

#include <Windows.h>

#include <cwctype>
#include <filesystem>

namespace skydiag::dump_tool {
namespace {

i18n::Language ParseLanguageTokenWide(std::wstring_view token)
{
  std::wstring t(token);
  for (auto& c : t) {
    c = static_cast<wchar_t>(towlower(c));
  }

  if (t == L"en" || t == L"eng" || t == L"english") {
    return i18n::Language::kEnglish;
  }
  if (t == L"ko" || t == L"kor" || t == L"korean" || t == L"kr" || t == L"korea") {
    return i18n::Language::kKorean;
  }
  return i18n::DefaultLanguage();
}

}  // namespace

std::wstring DumpToolIniPath()
{
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::filesystem::path p(buf, buf + n);
  return (p.parent_path() / L"SkyrimDiagDumpTool.ini").wstring();
}

DumpToolConfig LoadDumpToolConfig(std::wstring* err)
{
  DumpToolConfig cfg{};
  const std::wstring path = DumpToolIniPath();

  wchar_t lang[64]{};
  GetPrivateProfileStringW(L"SkyrimDiagDumpTool", L"Language", L"en", lang, 64, path.c_str());
  cfg.language = ParseLanguageTokenWide(lang);

  if (err) err->clear();
  return cfg;
}

bool SaveDumpToolConfig(const DumpToolConfig& cfg, std::wstring* err)
{
  const std::wstring path = DumpToolIniPath();
  const std::wstring code = std::wstring(i18n::LanguageCode(cfg.language));

  const BOOL ok = WritePrivateProfileStringW(L"SkyrimDiagDumpTool", L"Language", code.c_str(), path.c_str());
  if (!ok) {
    if (err) *err = L"WritePrivateProfileStringW failed: " + std::to_wstring(GetLastError());
    return false;
  }
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool
