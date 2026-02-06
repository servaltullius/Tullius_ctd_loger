#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {
namespace bucket {

inline std::wstring LowerTrimmed(std::wstring_view s)
{
  std::size_t begin = 0;
  while (begin < s.size()) {
    const wchar_t c = s[begin];
    if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') {
      break;
    }
    begin++;
  }

  std::size_t end = s.size();
  while (end > begin) {
    const wchar_t c = s[end - 1];
    if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') {
      break;
    }
    end--;
  }

  std::wstring out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; i++) {
    out.push_back(static_cast<wchar_t>(towlower(s[i])));
  }
  return out;
}

inline std::string NarrowAsciiFallback(std::wstring_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const wchar_t wc : s) {
    if (wc >= 0 && wc <= 0x7F) {
      out.push_back(static_cast<char>(wc));
    } else {
      out.push_back('?');
    }
  }
  return out;
}

inline std::uint64_t Fnv1a64(std::string_view s)
{
  std::uint64_t h = 14695981039346656037ull;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace bucket

inline std::wstring ComputeCrashBucketKey(
  std::uint32_t exceptionCode,
  std::wstring_view faultModule,
  const std::vector<std::wstring>& frames,
  std::size_t maxFrames = 6)
{
  std::ostringstream canonical;
  canonical << "exc=0x" << std::hex << std::nouppercase << exceptionCode << "|mod=";
  canonical << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(faultModule));

  const std::size_t n = std::min<std::size_t>(frames.size(), maxFrames);
  for (std::size_t i = 0; i < n; i++) {
    canonical << "|f" << i << "=";
    canonical << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(frames[i]));
  }

  const std::uint64_t hash = bucket::Fnv1a64(canonical.str());
  std::wstringstream wss;
  wss << L"CTD-" << std::hex << std::nouppercase << std::setw(16) << std::setfill(L'0') << hash;
  return wss.str();
}

}  // namespace skydiag::dump_tool

