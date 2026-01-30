#include "SkyrimDiag/Blackbox.h"

#include <Windows.h>

#include "SkyrimDiag/SharedMemory.h"

namespace skydiag::plugin {
namespace {

inline std::uint64_t QpcNow() noexcept
{
  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  return static_cast<std::uint64_t>(li.QuadPart);
}

void PushEventImpl(
  skydiag::EventType type,
  const skydiag::EventPayload& payload,
  std::uint16_t usedBytes,
  bool bypassFrozen) noexcept
{
  auto* shm = GetShared();
  if (!shm) {
    return;
  }

  if (!bypassFrozen && (shm->header.state_flags & skydiag::kState_Frozen) != 0u) {
    return;
  }

  if (usedBytes > sizeof(skydiag::EventPayload)) {
    usedBytes = sizeof(skydiag::EventPayload);
  }

  const auto idx = static_cast<std::uint32_t>(
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->header.write_index)) - 1);

  auto& e = shm->events[idx % shm->header.capacity];
  const std::uint32_t seq = idx * 2u;

  e.seq = seq | 1u;  // odd => writing
  e.tid = GetCurrentThreadId();
  e.qpc = QpcNow();
  e.type = static_cast<std::uint16_t>(type);
  e.size = usedBytes;
  e.payload = payload;
  e.seq = seq;  // even => committed
}

}  // namespace

void PushEvent(skydiag::EventType type, const skydiag::EventPayload& payload, std::uint16_t usedBytes) noexcept
{
  PushEventImpl(type, payload, usedBytes, /*bypassFrozen=*/false);
}

void PushEventAlways(skydiag::EventType type, const skydiag::EventPayload& payload, std::uint16_t usedBytes) noexcept
{
  PushEventImpl(type, payload, usedBytes, /*bypassFrozen=*/true);
}

}  // namespace skydiag::plugin

