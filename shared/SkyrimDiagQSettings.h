#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace skydiag {

namespace qsettings_detail {

inline void AppendUtf8(std::string& out, std::uint32_t cp)
{
  if (cp < 0x80u) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

inline bool IsSpace(char c)
{
  return c == '\r' || c == '\n' || c == ' ' || c == '\t';
}

inline void TrimInPlace(std::string& s)
{
  std::size_t end = s.size();
  while (end > 0 && IsSpace(s[end - 1])) {
    --end;
  }
  std::size_t begin = 0;
  while (begin < end && IsSpace(s[begin])) {
    ++begin;
  }
  s = s.substr(begin, end - begin);
}

inline int HexValue(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace qsettings_detail

// A value as QSettings writes it to an INI file (ModOrganizer.ini): QByteArray
// values are wrapped in @ByteArray(...), values with special characters are
// quoted, and backslashes, quotes and characters it does not write as-is are
// escaped (\\, \", \xNNNN; inside @ByteArray \xNN is one byte). The result is
// UTF-8 unless an @ByteArray held bytes in another encoding.
inline std::string DecodeQSettingsIniValue(std::string raw)
{
  using namespace qsettings_detail;
  if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    raw.erase(0, 3);
  }
  TrimInPlace(raw);

  bool byteArray = false;
  constexpr std::string_view kByteArrayPrefix = "@ByteArray(";
  if (raw.size() >= kByteArrayPrefix.size() + 1 && raw.rfind(kByteArrayPrefix, 0) == 0 && raw.back() == ')') {
    raw = raw.substr(kByteArrayPrefix.size(), raw.size() - kByteArrayPrefix.size() - 1);
    TrimInPlace(raw);
    byteArray = true;
  }

  if (raw.size() >= 2) {
    const char first = raw.front();
    const char last = raw.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      raw = raw.substr(1, raw.size() - 2);
      TrimInPlace(raw);
    }
  }

  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\' || i + 1 >= raw.size()) {
      out.push_back(raw[i]);
      continue;
    }
    const char next = raw[++i];
    if (next == 'x' || next == 'X') {
      std::uint32_t value = 0;
      std::size_t digits = 0;
      while (digits < (byteArray ? 2u : 4u) && i + 1 < raw.size() && HexValue(raw[i + 1]) >= 0) {
        value = value * 16u + static_cast<std::uint32_t>(HexValue(raw[++i]));
        ++digits;
      }
      if (digits == 0) {
        out += "\\x";
      } else if (byteArray) {
        out.push_back(static_cast<char>(value & 0xFFu));
      } else {
        AppendUtf8(out, value);
      }
      continue;
    }
    switch (next) {
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      default:
        out.push_back('\\');
        out.push_back(next);
        break;
    }
  }
  return out;
}

}  // namespace skydiag
