#pragma once

#include <cstdint>
#include <string>

namespace skydiag::protocol {

// Kernel object naming
inline constexpr wchar_t kKernelObjectPrefix[] = L"Local\\SkyrimDiag_";
inline constexpr wchar_t kKernelObjectSuffix_SharedMemory[] = L"_SHM";
inline constexpr wchar_t kKernelObjectSuffix_CrashEvent[] = L"_CRASH";
inline constexpr wchar_t kKernelObjectSuffix_HelperMutex[] = L"_HELPER_MUTEX";

// Custom minidump user stream types must be > MINIDUMP_STREAM_TYPE::LastReservedStream (0xffff).
inline constexpr std::uint32_t kMinidumpUserStream_Blackbox = 0x10000u + 0x5344u;  // arbitrary
inline constexpr std::uint32_t kMinidumpUserStream_WctJson = 0x10000u + 0x5743u;   // arbitrary
inline constexpr std::uint32_t kMinidumpUserStream_PluginInfo = 0x10000u + 0x504Cu;  // arbitrary "PL"

// Build a kernel object name from PID and suffix.
inline std::wstring MakeKernelName(std::uint32_t pid, const wchar_t* suffix)
{
  std::wstring name;
  name.reserve(64);
  name.append(kKernelObjectPrefix);
  name.append(std::to_wstring(pid));
  name.append(suffix);
  return name;
}

}  // namespace skydiag::protocol
