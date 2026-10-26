#pragma once

namespace skydiag::plugin {

// ADR-0022: records EventType::kCloseRequest when the player asks the game to
// close. Two hooks on the game window's thread: WH_GETMESSAGE sees the posted
// Alt+F4 key-down, SC_CLOSE, WM_CLOSE and WM_QUIT; WH_CALLWNDPROC the sent
// SC_CLOSE and WM_CLOSE (the X button, DefWindowProc's WM_CLOSE). They only
// look and always pass the message on (CallNextHookEx), so they change no
// message handling and do not touch the window procedure other plugins
// subclass. Call once the game window exists (kDataLoaded); false when it
// could not be found or both hooks were refused.
bool InstallCloseRequestHooks() noexcept;

// The game's own quit flag (Main::quitGame), read from the heartbeat task
// while the main loop still runs; records one kGameQuit when it becomes set.
// Does nothing until InstallCloseRequestHooks ran.
void PollGameQuitRequest() noexcept;

}  // namespace skydiag::plugin
