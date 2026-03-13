#include "SkyrimDiag/Blackbox.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "SkyrimDiag/Hash.h"
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

constexpr char ToLowerAscii(char ch) noexcept
{
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(ch + ('a' - 'A'));
  }
  return ch;
}

std::uint64_t HashLabel(std::string_view label) noexcept
{
  char lowerBuf[64]{};
  const std::size_t copyLen = std::min<std::size_t>(label.size(), sizeof(lowerBuf) - 1);
  for (std::size_t i = 0; i < copyLen; ++i) {
    lowerBuf[i] = ToLowerAscii(label[i]);
  }
  return skydiag::hash::Fnv1a64(std::string_view(lowerBuf, copyLen));
}

void PackShortText(std::string_view text, skydiag::EventPayload& payload) noexcept
{
  static_assert(sizeof(payload.b) + sizeof(payload.c) + sizeof(payload.d) == 24);
  char* dst = reinterpret_cast<char*>(&payload.b);
  std::memset(dst, 0, sizeof(payload.b) + sizeof(payload.c) + sizeof(payload.d));
  if (text.empty()) {
    return;
  }

  const std::size_t maxBytes = (sizeof(payload.b) + sizeof(payload.c) + sizeof(payload.d)) - 1;
  const std::size_t copyLen = std::min<std::size_t>(text.size(), maxBytes);
  std::memcpy(dst, text.data(), copyLen);
  dst[copyLen] = '\0';
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

void PushModuleLifecycleEvent(skydiag::EventType type, std::string_view moduleBasenameUtf8) noexcept
{
  if (type != skydiag::EventType::kModuleLoad &&
      type != skydiag::EventType::kModuleUnload) {
    return;
  }
  skydiag::EventPayload payload{};
  payload.a = HashLabel(moduleBasenameUtf8);
  PackShortText(moduleBasenameUtf8, payload);
  PushEvent(type, payload, sizeof(payload));
}

void PushThreadLifecycleEvent(
  skydiag::EventType type,
  std::uint32_t threadId,
  std::uint32_t activeThreadCount) noexcept
{
  if (type != skydiag::EventType::kThreadCreate &&
      type != skydiag::EventType::kThreadExit) {
    return;
  }
  skydiag::EventPayload payload{};
  payload.a = threadId;
  payload.b = activeThreadCount;
  if (auto* shm = GetShared()) {
    payload.c = shm->header.state_flags;
  }
  PushEvent(type, payload, sizeof(payload));
}

void PushFirstChanceExceptionEvent(
  std::uint32_t exceptionCode,
  std::uint32_t addressBucket,
  std::string_view moduleBasenameUtf8) noexcept
{
  skydiag::EventPayload payload{};
  payload.a = (static_cast<std::uint64_t>(addressBucket) << 32) | static_cast<std::uint64_t>(exceptionCode);
  PackShortText(moduleBasenameUtf8, payload);
  PushEvent(skydiag::EventType::kFirstChanceException, payload, sizeof(payload));
}

}  // namespace skydiag::plugin
