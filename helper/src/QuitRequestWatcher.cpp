#include "QuitRequestWatcher.h"

#include "SeqlockCopy.h"

namespace skydiag::helper::internal {

bool QuitRequestWatcher::Poll(const skydiag::SharedLayout* shm) noexcept
{
  if (seen_ || !shm || shm->header.magic != skydiag::kMagic) {
    return false;
  }
  const auto& header = shm->header;
  std::uint32_t capacity = header.capacity;
  if (capacity == 0u || capacity > skydiag::kEventCapacity) {
    capacity = skydiag::kEventCapacity;
  }
  const std::uint32_t writeIndex = ReadRingWriteIndex(header);
  const std::uint32_t oldest = writeIndex > capacity ? writeIndex - capacity : 0u;
  if (!started_ || next_ < oldest) {
    // First poll, or a lap behind: start at the oldest entry the ring holds.
    next_ = oldest;
    started_ = true;
  }
  for (; next_ < writeIndex; ++next_) {
    skydiag::BlackboxEvent event{};
    if (!CopyStableSeqlockEntry(&shm->events[next_ % capacity], &event)) {
      break;  // being written: read it on the next poll
    }
    // The plugin commits ring index i as seq i*2.
    const std::uint32_t committed = next_ * 2u;
    if (event.seq != committed) {
      if (event.seq < committed) {
        break;  // claimed but not written yet
      }
      continue;  // a later lap already took the slot
    }
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCloseRequest) &&
        IsQuitFlagRequest(event.payload.a)) {
      seen_ = true;
      kind_ = static_cast<skydiag::CloseRequestKind>(event.payload.a);
      ++next_;
      return true;
    }
  }
  return false;
}

}  // namespace skydiag::helper::internal
