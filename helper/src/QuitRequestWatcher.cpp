#include "QuitRequestWatcher.h"

namespace skydiag::helper::internal {

bool QuitRequestWatcher::Poll(const skydiag::SharedLayout* shm) noexcept
{
  if (seen_ || !cursor_.Begin(shm)) {
    return false;
  }
  skydiag::BlackboxEvent event{};
  std::uint32_t index = 0;
  while (cursor_.Next(shm, &event, &index)) {
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCloseRequest) &&
        skydiag::IsQuitFlagCloseRequest(event.payload.a)) {
      seen_ = true;
      kind_ = static_cast<skydiag::CloseRequestKind>(event.payload.a);
      return true;
    }
  }
  return false;
}

}  // namespace skydiag::helper::internal
