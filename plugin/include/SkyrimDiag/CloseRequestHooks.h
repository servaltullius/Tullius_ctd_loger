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

// True once InstallCloseRequestHooks ran: the heartbeat scheduler then wakes
// every 25 ms to call SampleCloseKeys, and otherwise sleeps its interval.
bool CloseKeySamplingOn() noexcept;

// The key sampler, every 25 ms on the heartbeat scheduler thread: records an
// alt_f4 close request when Alt and F4 become held together while the game
// window (captured at install, refreshed by PollGameQuitRequest) is the
// foreground window. GetAsyncKeyState
// is the system's key state, which DirectInput does not hide. Two key-state
// reads per call, the foreground window only while both are held; never
// touches Main.
void SampleCloseKeys() noexcept;

// From the crash handler, once a crash is recorded: Alt+F4 still held with
// the game in front records an alt_f4 close request (unless this press was
// already recorded) before the Crash event, for a crash in the very frame
// that handles Alt+F4. Only on a thread that is already a GUI thread
// (IsGUIThread(FALSE)), so the crash never converts one. No allocation, no
// logging.
void RecordCloseKeysAtCrash() noexcept;

// From the menu event sink (UI thread): a menu open as one quits
// (MessageBoxMenu, Main Menu, Console) opened or closed.
void NoteMenuForQuit(std::string_view menuName, bool opening) noexcept;

}  // namespace skydiag::plugin
