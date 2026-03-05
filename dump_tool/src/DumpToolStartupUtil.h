#pragma once
// Shared helpers for DumpToolCliMain.cpp and NativeApi.cpp.
// Windows-only: uses GetEnvironmentVariableW, GetModuleFileNameW.

#include <Windows.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::startup {

inline std::string ToAscii(std::wstring_view w)
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

inline bool ParseBoolText(std::wstring_view text, bool defaultValue)
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

inline bool ReadEnvBool(const wchar_t* key, bool defaultValue)
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

inline std::wstring ReadEnvString(const wchar_t* key)
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

inline std::wstring GetCurrentExeDir()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring_view(buf.data(), n)).parent_path().wstring();
}

}  // namespace skydiag::dump_tool::startup
