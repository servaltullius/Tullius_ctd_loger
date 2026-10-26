#pragma once

namespace skydiag::plugin {

// ADR-0022: records EventType::kCloseRequest when the player asks the game to
// close. A WH_CALLWNDPROC hook on the game window's thread sees the
// WM_SYSCOMMAND(SC_CLOSE) and WM_CLOSE that Alt+F4 and the X button send; it
// only looks and always passes the message on (CallNextHookEx), so it changes
// no message handling and does not touch the window procedure other plugins
// subclass. Call once the game window exists (kDataLoaded); false when it
// could not be found or the hook was refused.
bool InstallCloseRequestHooks() noexcept;

// The game's own quit flag (Main::quitGame), read from the heartbeat task
// while the main loop still runs; records one kGameQuit when it becomes set.
// Does nothing until InstallCloseRequestHooks ran.
void PollGameQuitRequest() noexcept;

}  // namespace skydiag::plugin
