#pragma once

#include <algorithm>
#include <cstdint>

namespace skydiag::plugin {

// When the plugin's watchdog starts the helper again (PluginMain's
// StartHelperWatchdogIfConfigured). The watchdog looks once a second whether
// a helper holds this game's singleton and feeds what it sees in here.
//
// A helper that ran for a while and then went away is started again at once:
// it has crash or freeze captures to take. A start that fails, is not
// confirmed, or leaves a helper that is gone again within kHealthyRunMs is a
// failed start: the next one waits, twice as long each time, and after
// kMaxFailedStarts in a row the watchdog stops for the rest of the session.
// Something on the machine ends the helper then (security software, a helper
// that cannot attach, a crash at startup), and starting it again every few
// seconds only costs the game a process launch each time. Before, a launch
// that was confirmed put the wait back to its minimum, so a helper that
// exited nine seconds after every start was started again about every ten
// seconds, for as long as the game ran.
class HelperWatchdogPolicy
{
public:
  static constexpr std::uint64_t kRetryMinMs = 1000;
  static constexpr std::uint64_t kRetryMaxMs = 30000;
  // The helper the plugin starts at load gets this long before the first look.
  static constexpr std::uint64_t kInitialGraceMs = 3000;
  // A helper that stayed this long had started properly. Its startup (attach,
  // preflight, first scans) is over within seconds.
  static constexpr std::uint64_t kHealthyRunMs = 60000;
  static constexpr std::uint32_t kMaxFailedStarts = 5;

  enum class Action : std::uint8_t {
    kNone,
    kLaunch,
  };

  // What the last Tick or NoteLaunch found, for the log; read it right after.
  enum class Event : std::uint8_t {
    kNone,
    kHelperExitedEarly,  // LastRunMs(), then waits RetryInMs()
    kStartFailed,        // waits RetryInMs()
    kGaveUp,             // FailedStarts() in a row
  };

  constexpr explicit HelperWatchdogPolicy(std::uint64_t nowMs) noexcept
    : nextAttemptMs_(nowMs + kInitialGraceMs)
  {
  }

  // Once a loop: whether a helper is there now. kLaunch asks for a start,
  // whose outcome goes to NoteLaunch.
  constexpr Action Tick(std::uint64_t nowMs, bool helperPresent) noexcept
  {
    event_ = Event::kNone;
    if (helperPresent) {
      if (!present_) {
        present_ = true;
        presentSinceMs_ = nowMs;
      }
      if (nowMs - presentSinceMs_ >= kHealthyRunMs) {
        failedStarts_ = 0;
        backoffMs_ = kRetryMinMs;
      }
      return Action::kNone;
    }

    if (present_) {
      // The helper that was running is gone.
      present_ = false;
      lastRunMs_ = nowMs - presentSinceMs_;
      if (lastRunMs_ >= kHealthyRunMs) {
        failedStarts_ = 0;
        backoffMs_ = kRetryMinMs;
        nextAttemptMs_ = 0;
      } else {
        NoteFailedStart(nowMs, Event::kHelperExitedEarly);
      }
    }
    if (gaveUp_ || (nextAttemptMs_ != 0 && nowMs < nextAttemptMs_)) {
      return Action::kNone;
    }
    return Action::kLaunch;
  }

  // After a start Tick asked for: confirmed when the new helper took the
  // singleton. A confirmed start is not yet a good one; Tick decides that
  // from how long the helper stays.
  constexpr void NoteLaunch(std::uint64_t nowMs, bool confirmed) noexcept
  {
    event_ = Event::kNone;
    if (confirmed) {
      present_ = true;
      presentSinceMs_ = nowMs;
      nextAttemptMs_ = 0;
      return;
    }
    NoteFailedStart(nowMs, Event::kStartFailed);
  }

  constexpr Event LastEvent() const noexcept { return event_; }
  constexpr bool GaveUp() const noexcept { return gaveUp_; }
  constexpr std::uint32_t FailedStarts() const noexcept { return failedStarts_; }
  constexpr std::uint64_t LastRunMs() const noexcept { return lastRunMs_; }
  constexpr std::uint64_t RetryInMs() const noexcept { return retryInMs_; }

private:
  constexpr void NoteFailedStart(std::uint64_t nowMs, Event event) noexcept
  {
    ++failedStarts_;
    if (failedStarts_ >= kMaxFailedStarts) {
      gaveUp_ = true;
      event_ = Event::kGaveUp;
      return;
    }
    event_ = event;
    retryInMs_ = backoffMs_;
    nextAttemptMs_ = nowMs + backoffMs_;
    backoffMs_ = std::min(backoffMs_ * 2u, kRetryMaxMs);
  }

  std::uint64_t backoffMs_ = kRetryMinMs;
  std::uint64_t nextAttemptMs_ = 0;  // 0: at once
  std::uint64_t presentSinceMs_ = 0;
  std::uint64_t lastRunMs_ = 0;
  std::uint64_t retryInMs_ = 0;
  std::uint32_t failedStarts_ = 0;
  bool present_ = false;
  bool gaveUp_ = false;
  Event event_ = Event::kNone;
};

}  // namespace skydiag::plugin
