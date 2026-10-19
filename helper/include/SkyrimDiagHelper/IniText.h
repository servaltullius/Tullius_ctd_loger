#pragma once

// SkyrimDiagHelper.ini is read here instead of through GetPrivateProfile*W.
// That API decodes a file without a UTF-16 BOM in the ANSI code page: a UTF-8
// BOM glues itself to the first section header, so every setting silently
// fell back to its default, and a UTF-8 OutputDir with Korean letters turned
// into mojibake on a CP949 system (the viewer reads the same file as UTF-8,
// so the two looked in different folders). The bytes are decoded first, and
// the lookups below reproduce what GetPrivateProfileStringW/IntW return on
// Windows 11 (probed 2026-10-19), so every existing value keeps its meaning.
//
// Portable: the ANSI code page fallback is supplied by the caller.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::helper {

enum class IniTextEncoding
{
  kEmpty,
  kUtf8Bom,
  kUtf16LeBom,
  kUtf16BeBom,
  kUtf16LeNoBom,
  kUtf8,
  kAnsi,
};

namespace ini_detail {

inline constexpr char32_t kReplacementChar = 0xFFFD;

inline void AppendCodePoint(std::wstring& out, char32_t cp)
{
  if constexpr (sizeof(wchar_t) == 2) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
      return;
    }
  }
  out.push_back(static_cast<wchar_t>(cp));
}

// Decodes UTF-8. Strict mode rejects overlong forms, surrogates, values above
// U+10FFFF and truncated sequences (as MB_ERR_INVALID_CHARS does); otherwise
// each bad sequence becomes U+FFFD.
inline std::optional<std::wstring> DecodeUtf8(std::string_view bytes, bool strict)
{
  std::wstring out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
      out.push_back(static_cast<wchar_t>(b0));
      ++i;
      continue;
    }
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t minCp = 0;
    if ((b0 & 0xE0) == 0xC0) {
      len = 2;
      cp = b0 & 0x1F;
      minCp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
      len = 3;
      cp = b0 & 0x0F;
      minCp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
      len = 4;
      cp = b0 & 0x07;
      minCp = 0x10000;
    }
    bool valid = len != 0 && i + len <= bytes.size();
    for (std::size_t k = 1; valid && k < len; ++k) {
      const auto bk = static_cast<unsigned char>(bytes[i + k]);
      if ((bk & 0xC0) != 0x80) {
        valid = false;
      } else {
        cp = (cp << 6) | (bk & 0x3F);
      }
    }
    if (valid && (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
      valid = false;
    }
    if (!valid) {
      if (strict) {
        return std::nullopt;
      }
      out.push_back(static_cast<wchar_t>(kReplacementChar));
      ++i;
      continue;
    }
    AppendCodePoint(out, cp);
    i += len;
  }
  return out;
}

inline std::wstring DecodeUtf16(std::string_view bytes, bool bigEndian)
{
  std::vector<std::uint16_t> units;
  units.reserve(bytes.size() / 2);
  for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
    const auto lo = static_cast<unsigned char>(bytes[bigEndian ? i + 1 : i]);
    const auto hi = static_cast<unsigned char>(bytes[bigEndian ? i : i + 1]);
    units.push_back(static_cast<std::uint16_t>(lo | (hi << 8)));
  }
  std::wstring out;
  out.reserve(units.size());
  if constexpr (sizeof(wchar_t) == 2) {
    for (const auto u : units) {
      out.push_back(static_cast<wchar_t>(u));
    }
  } else {
    for (std::size_t i = 0; i < units.size(); ++i) {
      const char32_t u = units[i];
      if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        out.push_back(static_cast<wchar_t>(0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00)));
        ++i;
      } else if (u >= 0xD800 && u <= 0xDFFF) {
        out.push_back(static_cast<wchar_t>(kReplacementChar));
      } else {
        out.push_back(static_cast<wchar_t>(u));
      }
    }
  }
  return out;
}

inline bool StartsWithBytes(std::string_view bytes, std::string_view prefix)
{
  return bytes.substr(0, prefix.size()) == prefix;
}

// GetPrivateProfileString trims these around lines, names and values.
inline bool IsIniSpace(wchar_t c)
{
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v' || c == 0x1A;
}

inline std::wstring_view TrimIniSpace(std::wstring_view s)
{
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && IsIniSpace(s[b])) {
    ++b;
  }
  while (e > b && IsIniSpace(s[e - 1])) {
    --e;
  }
  return s.substr(b, e - b);
}

inline wchar_t AsciiUpper(wchar_t c)
{
  return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

inline bool EqualsAsciiNoCase(std::wstring_view a, std::wstring_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (AsciiUpper(a[i]) != AsciiUpper(b[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace ini_detail

using IniAnsiDecoder = std::wstring (*)(std::string_view bytes);

// UTF-8 BOM, UTF-16LE/BE BOM, BOM-less UTF-16LE (which the Windows API also
// read), valid UTF-8, and otherwise the ANSI code page through decodeAnsi
// (Latin-1 when none is given).
inline std::wstring DecodeIniBytes(
  std::string_view bytes,
  IniAnsiDecoder decodeAnsi,
  IniTextEncoding* outEncoding = nullptr)
{
  IniTextEncoding encoding = IniTextEncoding::kEmpty;
  std::wstring text;
  if (bytes.empty()) {
    encoding = IniTextEncoding::kEmpty;
  } else if (ini_detail::StartsWithBytes(bytes, "\xEF\xBB\xBF")) {
    encoding = IniTextEncoding::kUtf8Bom;
    text = *ini_detail::DecodeUtf8(bytes.substr(3), /*strict=*/false);
  } else if (ini_detail::StartsWithBytes(bytes, "\xFF\xFE")) {
    encoding = IniTextEncoding::kUtf16LeBom;
    text = ini_detail::DecodeUtf16(bytes.substr(2), /*bigEndian=*/false);
  } else if (ini_detail::StartsWithBytes(bytes, "\xFE\xFF")) {
    encoding = IniTextEncoding::kUtf16BeBom;
    text = ini_detail::DecodeUtf16(bytes.substr(2), /*bigEndian=*/true);
  } else if (bytes.size() >= 2 && bytes.size() % 2 == 0 && bytes[0] != '\0' && bytes[1] == '\0') {
    encoding = IniTextEncoding::kUtf16LeNoBom;
    text = ini_detail::DecodeUtf16(bytes, /*bigEndian=*/false);
  } else if (auto utf8 = ini_detail::DecodeUtf8(bytes, /*strict=*/true)) {
    encoding = IniTextEncoding::kUtf8;
    text = std::move(*utf8);
  } else {
    encoding = IniTextEncoding::kAnsi;
    if (decodeAnsi) {
      text = decodeAnsi(bytes);
    } else {
      text.reserve(bytes.size());
      for (const char c : bytes) {
        text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
      }
    }
  }
  if (outEncoding) {
    *outEncoding = encoding;
  }
  return text;
}

// The sections and keys of a decoded INI file, parsed the way
// GetPrivateProfileString reads them: lines split at CR or LF; blank lines and
// lines starting with ';' skipped; "[name" up to the first ']' (or the line
// end) starts a section; "key=value" lines, trimmed around key and value, and
// lines without '=' or before the first section ignored. A NUL ends its line.
class IniDocument
{
public:
  static IniDocument Parse(std::wstring_view text)
  {
    IniDocument doc;
    std::size_t pos = 0;
    while (pos < text.size()) {
      std::size_t end = pos;
      while (end < text.size() && text[end] != L'\r' && text[end] != L'\n') {
        ++end;
      }
      std::wstring_view line = text.substr(pos, end - pos);
      pos = end;
      while (pos < text.size() && (text[pos] == L'\r' || text[pos] == L'\n')) {
        ++pos;
      }

      if (const auto nul = line.find(L'\0'); nul != std::wstring_view::npos) {
        line = line.substr(0, nul);
      }
      line = ini_detail::TrimIniSpace(line);
      if (line.empty() || line.front() == L';') {
        continue;
      }
      if (line.front() == L'[') {
        const auto close = line.find(L']');
        const auto name = line.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1);
        doc.sections_.push_back(Section{ std::wstring(ini_detail::TrimIniSpace(name)), {} });
        continue;
      }
      const auto eq = line.find(L'=');
      if (eq == std::wstring_view::npos || doc.sections_.empty()) {
        continue;
      }
      doc.sections_.back().entries.push_back(Entry{
        std::wstring(ini_detail::TrimIniSpace(line.substr(0, eq))),
        std::wstring(ini_detail::TrimIniSpace(line.substr(eq + 1))),
      });
    }
    return doc;
  }

  // The value of the first `key` in the first section named `section` (both
  // compared ASCII case-insensitively), with one pair of matching surrounding
  // quotes (" or ') removed. Like the Windows API, a later section with the
  // same name is not searched. Empty when the section or key is absent.
  std::optional<std::wstring> FindValue(std::wstring_view section, std::wstring_view key) const
  {
    section = ini_detail::TrimIniSpace(section);
    for (const auto& s : sections_) {
      if (!ini_detail::EqualsAsciiNoCase(s.name, section)) {
        continue;
      }
      for (const auto& e : s.entries) {
        if (!ini_detail::EqualsAsciiNoCase(e.key, key)) {
          continue;
        }
        std::wstring value = e.value;
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) {
          value = value.substr(1, value.size() - 2);
        }
        return value;
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

private:
  struct Entry
  {
    std::wstring key;
    std::wstring value;
  };
  struct Section
  {
    std::wstring name;
    std::vector<Entry> entries;
  };
  std::vector<Section> sections_;
};

// GetPrivateProfileStringW: the value, or `def` when the key is absent.
inline std::wstring ReadIniProfileString(
  const IniDocument& doc,
  std::wstring_view section,
  std::wstring_view key,
  std::wstring_view def)
{
  if (auto value = doc.FindValue(section, key)) {
    return std::move(*value);
  }
  return std::wstring(def);
}

// GetPrivateProfileIntW: `def` when the key is absent or empty; otherwise the
// value as RtlUnicodeStringToInteger reads it: leading blanks and one sign
// skipped, a lowercase 0x/0o/0b prefix selects the base, digits read up to
// the first character that is not one, wrapping modulo 2^32 ("1 ; note" is 1,
// "abc" is 0, "4294967295" is -1).
inline int ReadIniProfileInt(const IniDocument& doc, std::wstring_view section, std::wstring_view key, int def)
{
  const auto value = doc.FindValue(section, key);
  if (!value || value->empty()) {
    return def;
  }
  const std::wstring& s = *value;
  std::size_t i = 0;
  while (i < s.size() && s[i] <= L' ') {
    ++i;
  }
  bool negative = false;
  if (i < s.size() && (s[i] == L'+' || s[i] == L'-')) {
    negative = s[i] == L'-';
    ++i;
  }
  std::uint32_t base = 10;
  if (i + 1 < s.size() && s[i] == L'0') {
    if (s[i + 1] == L'x') {
      base = 16;
      i += 2;
    } else if (s[i + 1] == L'o') {
      base = 8;
      i += 2;
    } else if (s[i + 1] == L'b') {
      base = 2;
      i += 2;
    }
  }
  std::uint32_t total = 0;
  for (; i < s.size(); ++i) {
    const wchar_t c = s[i];
    std::uint32_t digit = 0;
    if (c >= L'0' && c <= L'9') {
      digit = static_cast<std::uint32_t>(c - L'0');
    } else if (c >= L'A' && c <= L'Z') {
      digit = static_cast<std::uint32_t>(c - L'A') + 10u;
    } else if (c >= L'a' && c <= L'z') {
      digit = static_cast<std::uint32_t>(c - L'a') + 10u;
    } else {
      break;
    }
    if (digit >= base) {
      break;
    }
    total = total * base + digit;
  }
  if (negative) {
    total = 0u - total;
  }
  return static_cast<int>(total);
}

}  // namespace skydiag::helper
