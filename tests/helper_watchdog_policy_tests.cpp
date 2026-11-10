#include "SkyrimDiag/HelperWatchdog.h"

#include <cassert>
#include <cstdint>
#include <vector>

using skydiag::plugin::HelperWatchdogPolicy;
using Action = HelperWatchdogPolicy::Action;
using Event = HelperWatchdogPolicy::Event;

namespace {

// The watchdog's loop over simulated time: one look a second, and a helper
// that takes the singleton at once and exits `lifetimeMs` after each start
// (0: it never takes the singleton). Returns when each start happened.
std::vector<std::uint64_t> Run(std::uint64_t lifetimeMs, std::uint64_t totalMs, HelperWatchdogPolicy* policyOut = nullptr)
{
  HelperWatchdogPolicy policy(0);
  std::vector<std::uint64_t> starts;
  // The helper the plugin starts at load.
  std::uint64_t helperUntilMs = lifetimeMs;
  for (std::uint64_t now = 0; now <= totalMs; now += 1000) {
    const bool present = now < helperUntilMs;
    if (policy.Tick(now, present) == Action::kLaunch) {
      starts.push_back(now);
      const bool confirmed = lifetimeMs != 0;
      helperUntilMs = confirmed ? now + lifetimeMs : 0;
      policy.NoteLaunch(now, confirmed);
    }
  }
  if (policyOut) {
    *policyOut = policy;
  }
  return starts;
}

// The case behind the change: a helper that exits nine seconds after every
// start was started again about every ten seconds for as long as the game
// ran (13 times in two minutes), each start a process launch from the game.
void Test_AHelperThatKeepsExitingEarlyIsGivenUpOn()
{
  HelperWatchdogPolicy policy(0);
  const auto starts = Run(9000, 600000, &policy);
  // The load-time helper and three restarts fail (1 s, 2 s, 4 s, 8 s waits
  // before each next one); the fifth early exit in a row ends it.
  assert(starts.size() == HelperWatchdogPolicy::kMaxFailedStarts - 1u);
  assert(policy.GaveUp() && policy.FailedStarts() == HelperWatchdogPolicy::kMaxFailedStarts);
  // The waits grow: 9 s of life, then 1, 2, 4, 8 s (a look comes once a second).
  assert(starts[0] == 10000 && starts[1] == 21000 && starts[2] == 34000 && starts[3] == 51000);
  for (const auto start : starts) {
    assert(start < 120000 && "nothing is started once it has given up");
  }
}

void Test_AHelperThatNeverStartsIsGivenUpOn()
{
  HelperWatchdogPolicy policy(0);
  const auto starts = Run(0, 600000, &policy);
  // Nothing is there after the grace for the load-time helper, and each
  // failed start doubles the wait: 3 s, then 1, 2, 4, 8 s later.
  assert(starts.size() == HelperWatchdogPolicy::kMaxFailedStarts);
  assert(starts[0] == 3000 && starts[1] == 4000 && starts[2] == 6000 && starts[3] == 10000 && starts[4] == 18000);
  assert(policy.GaveUp());
}

void Test_AHelperThatRanForAWhileIsRestartedAtOnce()
{
  HelperWatchdogPolicy policy(0);
  assert(policy.Tick(1000, true) == Action::kNone);
  assert(policy.Tick(200000, true) == Action::kNone);
  // Gone after more than three minutes: started again at the next look.
  assert(policy.Tick(201000, false) == Action::kLaunch && policy.LastEvent() == Event::kNone);
  policy.NoteLaunch(201500, true);
  assert(policy.LastEvent() == Event::kNone && policy.FailedStarts() == 0u && !policy.GaveUp());
  assert(policy.Tick(202000, true) == Action::kNone);

  // And every time, however often: only early exits in a row are counted.
  const auto starts = Run(90000, 1800000, &policy);
  assert(starts.size() == 20u && !policy.GaveUp() && policy.FailedStarts() == 0u);
}

void Test_EarlyExitsAreForgivenOnceAHelperStays()
{
  HelperWatchdogPolicy policy(0);
  // Two early exits.
  assert(policy.Tick(3000, true) == Action::kNone);
  assert(policy.Tick(5000, false) == Action::kNone);
  assert(policy.LastEvent() == Event::kHelperExitedEarly && policy.LastRunMs() == 2000 && policy.RetryInMs() == 1000 &&
         policy.FailedStarts() == 1u);
  assert(policy.Tick(6000, false) == Action::kLaunch);
  policy.NoteLaunch(6500, true);
  assert(policy.Tick(8000, false) == Action::kNone);
  assert(policy.LastEvent() == Event::kHelperExitedEarly && policy.RetryInMs() == 2000 && policy.FailedStarts() == 2u);
  assert(policy.Tick(9000, false) == Action::kNone && "still waiting");
  assert(policy.Tick(10000, false) == Action::kLaunch);
  policy.NoteLaunch(10500, true);
  // This one stays: a minute later the count and the wait are back to the start.
  assert(policy.Tick(40000, true) == Action::kNone && policy.FailedStarts() == 2u);
  assert(policy.Tick(71000, true) == Action::kNone && policy.FailedStarts() == 0u);
  // An early exit long after that starts counting from one, with the shortest wait.
  assert(policy.Tick(72000, false) == Action::kLaunch);
  policy.NoteLaunch(72500, true);
  assert(policy.Tick(75000, false) == Action::kNone);
  assert(policy.LastEvent() == Event::kHelperExitedEarly && policy.FailedStarts() == 1u && policy.RetryInMs() == 1000);
}

void Test_AnUnconfirmedStartWaitsAndIsCounted()
{
  HelperWatchdogPolicy policy(0);
  assert(policy.Tick(1000, false) == Action::kNone && "the load-time helper's grace");
  assert(policy.Tick(3000, false) == Action::kLaunch);
  policy.NoteLaunch(4500, false);
  assert(policy.LastEvent() == Event::kStartFailed && policy.RetryInMs() == 1000 && policy.FailedStarts() == 1u);
  assert(policy.Tick(5000, false) == Action::kNone);
  assert(policy.Tick(5500, false) == Action::kLaunch);
  policy.NoteLaunch(7000, false);
  assert(policy.RetryInMs() == 2000);
  // The wait is capped (it would take more failures than are allowed to reach
  // the cap, so the cap is checked on its own).
  assert(HelperWatchdogPolicy::kRetryMaxMs == 30000);
}

void Test_GivingUpIsSaidOnce()
{
  HelperWatchdogPolicy policy(0);
  std::uint64_t now = 3000;
  std::uint32_t gaveUpEvents = 0;
  for (int i = 0; i < 40; ++i, now += 31000) {
    if (policy.Tick(now, false) == Action::kLaunch) {
      policy.NoteLaunch(now + 1500, false);
      gaveUpEvents += policy.LastEvent() == Event::kGaveUp ? 1u : 0u;
    } else {
      assert(policy.LastEvent() == Event::kNone);
    }
  }
  assert(policy.GaveUp() && gaveUpEvents == 1u);
}

}  // namespace

int main()
{
  Test_AHelperThatKeepsExitingEarlyIsGivenUpOn();
  Test_AHelperThatNeverStartsIsGivenUpOn();
  Test_AHelperThatRanForAWhileIsRestartedAtOnce();
  Test_EarlyExitsAreForgivenOnceAHelperStays();
  Test_AnUnconfirmedStartWaitsAndIsCounted();
  Test_GivingUpIsSaidOnce();
  return 0;
}
