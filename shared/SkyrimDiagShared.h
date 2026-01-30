#pragma once

#include <Windows.h>

#include <cstdint>
#include <type_traits>

namespace skydiag {

inline constexpr std::uint32_t kMagic = 0x53444941u;  // 'SDIA'
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kEventCapacity = 1u << 16;  // 65536
inline constexpr std::uint32_t kResourceCapacity = 256;
inline constexpr std::uint32_t kResourcePathMaxBytes = 260;  // UTF-8, null-terminated (best-effort)

enum class EventType : std::uint16_t {
  kInvalid = 0,

  kSessionStart = 1,
  kHeartbeat = 2,

  kMenuOpen = 10,
  kMenuClose = 11,

  kLoadStart = 20,
  kLoadEnd = 21,

  kCellChange = 30,
  kNote = 40,
  kPerfHitch = 50,  // long main-thread stall / stutter

  kCrash = 100,
  kHangMark = 200,
};

enum StateFlags : std::uint32_t {
  kState_None = 0,
  kState_Frozen = 1u << 0,   // stop writing blackbox after crash mark
  kState_Loading = 1u << 1,  // loading screen/menu detected
  kState_InMenu = 1u << 2,   // any menu open detected
};

struct EventPayload {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;
  std::uint64_t d = 0;
};

// Seqlock-style: seq odd=writing, even=committed
struct BlackboxEvent {
  volatile std::uint32_t seq = 0;
  std::uint32_t tid = 0;
  std::uint64_t qpc = 0;
  std::uint16_t type = 0;
  std::uint16_t size = 0;
  std::uint32_t reserved = 0;
  EventPayload payload{};
};

static_assert(std::is_trivially_copyable_v<BlackboxEvent>);

struct ResourceEntry {
  // Seqlock-style: seq odd=writing, even=committed
  volatile std::uint32_t seq = 0;
  std::uint32_t tid = 0;
  std::uint64_t qpc = 0;
  std::uint64_t path_hash = 0;  // FNV1a64 of normalized path (best-effort)
  char path_utf8[kResourcePathMaxBytes]{};  // best-effort, may be truncated
};

static_assert(std::is_trivially_copyable_v<ResourceEntry>);

struct ResourceLog {
  volatile std::uint32_t write_index = 0;  // monotonically increases
  std::uint32_t reserved = 0;
  ResourceEntry entries[kResourceCapacity]{};
};

static_assert(std::is_trivially_copyable_v<ResourceLog>);

struct CrashInfo {
  std::uint32_t exception_code = 0;
  std::uint32_t faulting_tid = 0;
  std::uint64_t exception_addr = 0;

  // Best-effort copies for an out-of-proc minidump exception stream.
  EXCEPTION_RECORD exception_record{};
  CONTEXT context{};
};

struct SharedHeader {
  std::uint32_t magic = kMagic;
  std::uint32_t version = kVersion;
  std::uint32_t pid = 0;
  std::uint32_t capacity = kEventCapacity;

  std::uint64_t qpc_freq = 0;
  std::uint64_t start_qpc = 0;

  volatile std::uint64_t last_heartbeat_qpc = 0;  // updated only on main thread
  volatile std::uint32_t state_flags = 0;

  volatile std::uint32_t write_index = 0;  // monotonically increases
  volatile std::uint32_t crash_seq = 0;    // increments on crash
  volatile std::uint32_t hang_seq = 0;     // helper can bump when it takes hang dump

  CrashInfo crash{};
};

struct SharedLayout {
  SharedHeader header{};
  BlackboxEvent events[kEventCapacity]{};
  ResourceLog resources{};
};

static_assert(std::is_trivially_copyable_v<SharedLayout>);

}  // namespace skydiag
