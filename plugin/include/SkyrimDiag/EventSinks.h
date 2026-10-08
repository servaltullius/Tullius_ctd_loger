#pragma once

namespace skydiag::plugin {

bool RegisterEventSinks(bool logMenus);

// Re-derives kState_InMenu from the UI menu counters. Runs on the UI thread
// with each heartbeat so the flag settles even when menu events are off.
void RefreshInMenuFlag() noexcept;

}  // namespace skydiag::plugin

