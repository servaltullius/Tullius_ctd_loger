#pragma once

#include <cstdint>
#include <string_view>

#include "SkyrimDiagCloseRequest.h"
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

// ADR-0022: a, the CloseRequestKind; b, the window message (0 when none);
// c, its wParam (or the QuitFlagReason); d, the CloseRequestSource. Written
// even while a crash record is frozen: the rule only counts requests before
// a crash, and one after it must not be lost for the next crash.
void PushCloseRequestEvent(
  skydiag::CloseRequestKind kind,
  std::uint32_t message,
  std::uint64_t wParam,
  std::uint64_t source) noexcept;

inline void Note(std::uint64_t tag, std::uint64_t a = 0, std::uint64_t b = 0) noexcept
{
  skydiag::EventPayload p{};
  p.a = tag;
  p.b = a;
  p.c = b;
  PushEvent(skydiag::EventType::kNote, p, sizeof(p));
}

}  // namespace skydiag::plugin
