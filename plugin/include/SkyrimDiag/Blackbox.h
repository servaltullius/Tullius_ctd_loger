#pragma once

#include <cstdint>
#include <string_view>

#include "SkyrimDiagShared.h"

namespace skydiag::plugin {

void PushEvent(
  skydiag::EventType type,
  const skydiag::EventPayload& payload,
  std::uint16_t usedBytes = sizeof(skydiag::EventPayload)) noexcept;

// For crash/hang markers: bypasses the frozen check.
void PushEventAlways(
  skydiag::EventType type,
  const skydiag::EventPayload& payload,
  std::uint16_t usedBytes = sizeof(skydiag::EventPayload)) noexcept;

void PushModuleLifecycleEvent(
  skydiag::EventType type,
  std::string_view moduleBasenameUtf8) noexcept;

void PushThreadLifecycleEvent(
  skydiag::EventType type,
  std::uint32_t threadId,
  std::uint32_t activeThreadCount = 0) noexcept;

void PushFirstChanceExceptionEvent(
  std::uint32_t exceptionCode,
  std::uint32_t addressBucket,
  std::string_view moduleBasenameUtf8) noexcept;

inline void Note(std::uint64_t tag, std::uint64_t a = 0, std::uint64_t b = 0) noexcept
{
  skydiag::EventPayload p{};
  p.a = tag;
  p.b = a;
  p.c = b;
  PushEvent(skydiag::EventType::kNote, p, sizeof(p));
}

}  // namespace skydiag::plugin
