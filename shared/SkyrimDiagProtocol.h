#pragma once

#include <cstdint>
#include <string>

namespace skydiag::protocol {

// Kernel object naming
inline constexpr wchar_t kKernelObjectPrefix[] = L"Local\\SkyrimDiag_";
inline constexpr wchar_t kKernelObjectSuffix_SharedMemory[] = L"_SHM";
inline constexpr wchar_t kKernelObjectSuffix_CrashEvent[] = L"_CRASH";
inline constexpr wchar_t kKernelObjectSuffix_HelperMutex[] = L"_HELPER_MUTEX";
// ADR-0032. The plugin creates and sets _UNHANDLED when an exception reaches
// the process's top-level exception filter, i.e. nothing handled it. The
// helper creates _DUMP_SETTLED and sets it when a crash dump write is over;
// the plugin holds the crashing thread until then. Both are manual-reset and
// are reset by the helper when it lets go of the incident.
inline constexpr wchar_t kKernelObjectSuffix_UnhandledEvent[] = L"_UNHANDLED";
inline constexpr wchar_t kKernelObjectSuffix_DumpSettledEvent[] = L"_DUMP_SETTLED";

// Custom minidump user stream types must be > MINIDUMP_STREAM_TYPE::LastReservedStream (0xffff).
inline constexpr std::uint32_t kMinidumpUserStream_Blackbox = 0x10000u + 0x5344u;  // arbitrary
inline constexpr std::uint32_t kMinidumpUserStream_WctJson = 0x10000u + 0x5743u;   // arbitrary
inline constexpr std::uint32_t kMinidumpUserStream_PluginInfo = 0x10000u + 0x504Cu;  // arbitrary "PL"
// JSON: the game process's GPU memory per adapter at capture time ("GM").
inline constexpr std::uint32_t kMinidumpUserStream_GpuMemory = 0x10000u + 0x474Du;
// JSON: the hook stubs the helper read in the game's executable private
// memory while it ran (ADR-0024, "HP").
inline constexpr std::uint32_t kMinidumpUserStream_HookPages = 0x10000u + 0x4850u;
// JSON: the display drivers' events in the Windows System log from the game's
// start to a freeze or manual capture (ADR-0026, "GD").
inline constexpr std::uint32_t kMinidumpUserStream_GpuDriverEvents = 0x10000u + 0x4744u;
// Raw bytes: the game version's Address Library file as the game loads it
// (Data\SKSE\Plugins\versionlib-1-6-1170-0.bin, version-1-5-97-0.bin), so
// game frames get "ID 29924+0x6bc" labels (ADR-0027, "AL").
inline constexpr std::uint32_t kMinidumpUserStream_AddressLibrary = 0x10000u + 0x414Cu;

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
