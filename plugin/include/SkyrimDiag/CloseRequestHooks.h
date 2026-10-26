#pragma once

#include <string_view>

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
// while the main loop still runs; records one close request when it becomes
// set, its kind from QuitFlagCloseRequestKind (Alt+F4 held, a quit menu, or
// a forced quit). Skyrim takes Alt+F4 through DirectInput, so this is the
// only signal Alt+F4 gives. Does nothing until InstallCloseRequestHooks ran.
void PollGameQuitRequest() noexcept;

// Notes Alt+F4 held (GetAsyncKeyState, which DirectInput does not hide), so
// a quit flag seen a little later still knows. Called every 25 ms from the
// heartbeat scheduler thread; two key-state reads.
void SampleCloseKeys() noexcept;

// From the menu event sink (UI thread): a menu one quits from opened or
// closed.
void NoteMenuForQuit(std::string_view menuName, bool opening) noexcept;

}  // namespace skydiag::plugin
