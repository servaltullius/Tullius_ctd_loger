#pragma once

#include <cstdint>
#include <cwchar>
#include <string>

namespace skydiag::helper::internal {

inline std::wstring Hex32(std::uint32_t v)
{
  wchar_t buf[11]{};
  std::swprintf(buf, 11, L"0x%08X", static_cast<unsigned int>(v));
  return buf;
}

inline std::wstring Hex64(std::uint64_t v)
{
  wchar_t buf[19]{};
  std::swprintf(buf, 19, L"0x%016llX", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace skydiag::helper::internal
