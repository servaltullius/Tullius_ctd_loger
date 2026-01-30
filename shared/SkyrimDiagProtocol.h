#pragma once

#include <cstdint>

namespace skydiag::protocol {

// Kernel object naming
inline constexpr wchar_t kKernelObjectPrefix[] = L"Local\\SkyrimDiag_";
inline constexpr wchar_t kKernelObjectSuffix_SharedMemory[] = L"_SHM";
inline constexpr wchar_t kKernelObjectSuffix_CrashEvent[] = L"_CRASH";

// Custom minidump user stream types must be > MINIDUMP_STREAM_TYPE::LastReservedStream (0xffff).
inline constexpr std::uint32_t kMinidumpUserStream_Blackbox = 0x10000u + 0x5344u;  // arbitrary
inline constexpr std::uint32_t kMinidumpUserStream_WctJson = 0x10000u + 0x5743u;   // arbitrary

}  // namespace skydiag::protocol

