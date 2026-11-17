#pragma once

#include <cstdint>

#include "SeqlockCopy.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

// Follows the plugin's blackbox ring across polls: each pass reads the entries
// committed since the previous one, oldest first. The first pass starts at the
// oldest entry the ring still holds, so a helper started again reads what is
// left of the session. Shared by the helper's ring watchers (SaveLoadWatcher,
// QuitRequestWatcher).
class BlackboxCursor
{
public:
  // Starts a pass over what is written now. False when `shm` is not the
  // plugin's layout. `lapped` (optional) is set when the ring went a lap past
  // the cursor since the previous pass, so entries were lost.
  bool Begin(const skydiag::SharedLayout* shm, bool* lapped = nullptr) noexcept
  {
    if (lapped) {
      *lapped = false;
    }
    if (!shm || shm->header.magic != skydiag::kMagic) {
      return false;
    }
    capacity_ = shm->header.capacity;
    if (capacity_ == 0u || capacity_ > skydiag::kEventCapacity) {
      capacity_ = skydiag::kEventCapacity;
    }
    end_ = ReadRingWriteIndex(shm->header);
    const std::uint32_t oldest = end_ > capacity_ ? end_ - capacity_ : 0u;
    if (!started_ || next_ < oldest) {
      if (started_ && lapped) {
        *lapped = true;
      }
      next_ = oldest;
      started_ = true;
    }
    return true;
  }

  // The pass's next committed entry and its ring index. False at the end of
  // the pass, or at an entry that is claimed or being written: the next pass
  // reads it.
  bool Next(const skydiag::SharedLayout* shm, skydiag::BlackboxEvent* event, std::uint32_t* index) noexcept
  {
    while (next_ < end_) {
      if (!CopyStableSeqlockEntry(&shm->events[next_ % capacity_], event)) {
        return false;  // being written
      }
      // The plugin commits ring index i as seq i*2.
      const std::uint32_t committed = next_ * 2u;
      if (event->seq != committed) {
        if (event->seq < committed) {
          return false;  // claimed but not written yet
        }
        ++next_;  // a later lap already took the slot
        continue;
      }
      *index = next_++;
      return true;
    }
    return false;
  }

private:
  bool started_ = false;  // next_ is set
  std::uint32_t next_ = 0;
  std::uint32_t end_ = 0;
  std::uint32_t capacity_ = skydiag::kEventCapacity;
};

}  // namespace skydiag::helper::internal
