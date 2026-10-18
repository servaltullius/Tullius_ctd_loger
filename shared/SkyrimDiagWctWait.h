#pragma once

// Longest-waiter selection over a Wait Chain Traversal (WCT) capture, shared by
// the helper that records the capture and the dump tool that reads it back.
//
// Deliberately free of <Windows.h> so the host-independent unit tests can use
// the same rule instead of keeping a parallel copy that could drift apart.

#include <cstdint>

namespace skydiag {

// WCT_OBJECT_STATUS::WctStatusBlocked (wct.h), as the helper writes it.
inline constexpr std::uint32_t kWctObjectStatusBlocked = 3u;

// WAITCHAIN_NODE_INFO.ThreadObject.WaitTime is the system tick count
// (KeTickCount, 15.625 ms per tick) at which the thread last entered a wait,
// not how long it has waited: every thread of one capture, running ones
// included, carries a value of the same magnitude, and a thread that stays
// blocked keeps its value from one pass to the next. The longest waiter is
// therefore the blocked thread with the earliest non-zero value.
//
// Anything that suspends the whole process (a minidump writer, Crash Logger's
// thread dump) restarts the wait of every thread at the same tick, so several
// blocked threads often share the earliest value. Then no single thread is the
// longest waiter, and tid() stays 0.
class WctLongestWaiter
{
public:
  void Observe(std::uint32_t tid, std::uint32_t objectStatus, std::uint64_t waitStartTick) noexcept
  {
    if (tid == 0u || objectStatus != kWctObjectStatusBlocked || waitStartTick == 0u) {
      return;
    }
    if (count_ == 0u || waitStartTick < earliest_) {
      earliest_ = waitStartTick;
      tid_ = tid;
      count_ = 1u;
    } else if (waitStartTick == earliest_ && tid != tid_) {
      ++count_;
    }
  }

  // 0 when no blocked thread reported a wait start, or several share the earliest.
  std::uint32_t tid() const noexcept { return count_ == 1u ? tid_ : 0u; }
  std::uint64_t wait_start_tick() const noexcept { return count_ == 1u ? earliest_ : 0u; }

private:
  std::uint64_t earliest_ = 0u;
  std::uint32_t tid_ = 0u;
  std::uint32_t count_ = 0u;
};

}  // namespace skydiag
