#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper {

// The masking the dump tool applies to its redacted summaries
// (OutputWriterInternals.cpp MaybeRedactPath / RedactSymbolSearchPath), for
// files the helper writes that users attach to reports: an absolute path
// keeps only its last element, so user and folder names stay out.

inline bool LooksLikeAbsolutePathForRedaction(std::wstring_view path)
{
  if (path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 &&
    ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'));
}

// "C:\Users\A\Symbols" -> "<redacted>\Symbols"; anything that is not an
// absolute path comes back unchanged.
inline std::wstring RedactPath(std::wstring_view path)
{
  if (path.empty() || !LooksLikeAbsolutePathForRedaction(path)) {
    return std::wstring(path);
  }
  const std::wstring filename = std::filesystem::path(path).filename().wstring();
  if (filename.empty()) {
    return L"<redacted>";
  }
  return L"<redacted>\\" + filename;
}

// A DbgHelp search path is ';'-separated elements, each a directory or a
// "srv*<cache>*<url>" / "cache*<dir>" chain, and may come from
// _NT_SYMBOL_PATH or SKYRIMDIAG_SYMBOL_PATH: every absolute directory in it
// is redacted, keeping the separators, blanks and URLs.
inline std::wstring RedactSymbolSearchPath(std::wstring_view searchPath)
{
  std::wstring out;
  out.reserve(searchPath.size());
  std::size_t start = 0;
  for (;;) {
    const std::size_t sep = searchPath.find_first_of(L";*", start);
    const std::size_t stop = sep == std::wstring_view::npos ? searchPath.size() : sep;
    const std::wstring_view element = searchPath.substr(start, stop - start);
    std::size_t lead = 0;
    while (lead < element.size() && (element[lead] == L' ' || element[lead] == L'\t')) {
      ++lead;
    }
    out.append(element.substr(0, lead));
    out += RedactPath(element.substr(lead));
    if (sep == std::wstring_view::npos) {
      break;
    }
    out.push_back(searchPath[sep]);
    start = sep + 1;
  }
  return out;
}

}  // namespace skydiag::helper
