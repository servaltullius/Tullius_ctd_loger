#pragma once

#ifdef _WIN32
#include <Windows.h>

#include <winevt.h>
#endif

#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

#ifdef _WIN32
class EvtHandle
{
public:
  EvtHandle() = default;
  explicit EvtHandle(EVT_HANDLE handle) : handle_(handle) {}
  EvtHandle(const EvtHandle&) = delete;
  EvtHandle& operator=(const EvtHandle&) = delete;
  EvtHandle(EvtHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  EvtHandle& operator=(EvtHandle&& other) noexcept
  {
    if (this != &other) {
      Reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  ~EvtHandle() { Reset(); }
  EVT_HANDLE get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
  void Reset() noexcept
  {
    if (handle_) {
      EvtClose(handle_);
      handle_ = nullptr;
    }
  }
  EVT_HANDLE handle_ = nullptr;
};

inline std::wstring RenderEventXml(EVT_HANDLE event)
{
  DWORD used = 0;
  DWORD properties = 0;
  if (EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &properties) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0u) {
    return {};
  }
  std::wstring xml(used / sizeof(wchar_t) + 1u, L'\0');
  if (!EvtRender(nullptr, event, EvtRenderEventXml, used, xml.data(), &used, &properties)) {
    return {};
  }
  xml.resize(wcsnlen(xml.c_str(), xml.size()));
  return xml;
}
#endif

enum class XmlControlCharMode
{
  // GpuDriverEvents default: numeric refs < 0x20 become a space (folded by Tidy).
  kReplaceWithSpace,
  // WerExitEvent: strip 0x00..0x1f and 0x7f numeric refs along with raw control chars.
  kStrip,
};

inline std::wstring UnescapeXml(
  std::wstring_view text,
  XmlControlCharMode controlCharMode = XmlControlCharMode::kReplaceWithSpace)
{
  std::wstring out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != L'&') {
      out.push_back(text[i]);
      continue;
    }
    const auto end = text.find(L';', i);
    if (end == std::wstring_view::npos || end - i > 10u) {
      out.push_back(text[i]);
      continue;
    }
    const auto entity = text.substr(i + 1u, end - i - 1u);
    if (entity.size() > 1u && entity[0] == L'#') {
      // &#13; or &#x41;: control characters either become spaces (for Tidy) or
      // are stripped, depending on controlCharMode.
      const bool hex = entity[1] == L'x' || entity[1] == L'X';
      const std::wstring digits(entity.substr(hex ? 2u : 1u));
      wchar_t* stop = nullptr;
      const unsigned long code = digits.empty() ? 0ul : std::wcstoul(digits.c_str(), &stop, hex ? 16 : 10);
      if (!digits.empty() && stop && *stop == L'\0' && code <= 0x10FFFFul) {
        if (code < 0x20ul) {
          if (controlCharMode == XmlControlCharMode::kReplaceWithSpace) {
            out.push_back(L' ');
          }
        } else if (code == 0x7Ful && controlCharMode == XmlControlCharMode::kStrip) {
          // Strip DEL numeric character reference (&#127; / &#x7f;).
        } else if (code <= 0xFFFFul) {
          out.push_back(static_cast<wchar_t>(code));
        } else {
          const unsigned long v = code - 0x10000ul;
          out.push_back(static_cast<wchar_t>(0xD800ul + (v >> 10)));
          out.push_back(static_cast<wchar_t>(0xDC00ul + (v & 0x3FFul)));
        }
        i = end;
        continue;
      }
    }
    wchar_t c = 0;
    if (entity == L"amp") {
      c = L'&';
    } else if (entity == L"lt") {
      c = L'<';
    } else if (entity == L"gt") {
      c = L'>';
    } else if (entity == L"quot") {
      c = L'"';
    } else if (entity == L"apos") {
      c = L'\'';
    }
    if (c == 0) {
      out.push_back(text[i]);
      continue;
    }
    out.push_back(c);
    i = end;
  }
  return out;
}

// The value of attr='...' (or "...") inside the first tag that starts with `tag`.
inline std::wstring XmlAttributeOf(std::wstring_view xml, std::wstring_view tag, std::wstring_view attr)
{
  const auto at = xml.find(tag);
  if (at == std::wstring_view::npos) {
    return {};
  }
  const auto close = xml.find(L'>', at);
  const auto inside = xml.substr(at, close == std::wstring_view::npos ? std::wstring_view::npos : close - at);
  // " Name=", so EventSourceName= is not taken for it.
  const std::wstring key = L" " + std::wstring(attr) + L"=";
  const auto name = inside.find(key);
  if (name == std::wstring_view::npos || name + key.size() >= inside.size()) {
    return {};
  }
  const wchar_t quote = inside[name + key.size()];
  const auto start = name + key.size() + 1u;
  const auto end = inside.find(quote, start);
  return end == std::wstring_view::npos ? std::wstring{} : UnescapeXml(inside.substr(start, end - start));
}

// The text of <tag ...>text</tag>.
inline std::wstring XmlTextOf(std::wstring_view xml, std::wstring_view tag)
{
  const auto at = xml.find(L"<" + std::wstring(tag));
  if (at == std::wstring_view::npos) {
    return {};
  }
  const auto open = xml.find(L'>', at);
  if (open == std::wstring_view::npos || xml[open - 1u] == L'/') {
    return {};
  }
  const auto end = xml.find(L"</" + std::wstring(tag) + L">", open);
  return end == std::wstring_view::npos ? std::wstring{} : UnescapeXml(xml.substr(open + 1u, end - open - 1u));
}

}  // namespace skydiag::helper::internal
