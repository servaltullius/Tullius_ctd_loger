#pragma once

#include <cstdint>

#include "BlackboxCursor.h"
#include "SkyrimDiagCloseRequest.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper::internal {

// ADR-0035: follows the blackbox ring for the close request the plugin
// records when the game's quit flag (Main::quitGame) becomes set: game_quit
// (Quit in a menu, the console's qqq) or forced_quit. Once the flag is set the
// main loop ends and the game shuts down; it does not come back, so a freeze
// from then on is its shutdown. A long shutdown can push the request out of
// the ring, so it is latched as soon as it is read.
class QuitRequestWatcher
{
public:
  // Reads the ring entries written since the last poll; true when this poll
  // found the first quit request of the session. The first poll reads what
  // the ring still holds, so a helper started again finds one already made.
  bool Poll(const skydiag::SharedLayout* shm) noexcept;

  bool Seen() const noexcept { return seen_; }
  skydiag::CloseRequestKind Kind() const noexcept { return kind_; }

private:
  BlackboxCursor cursor_{};
  bool seen_ = false;
  skydiag::CloseRequestKind kind_ = skydiag::CloseRequestKind::kNone;
};

}  // namespace skydiag::helper::internal
