#include "SkyrimDiagHelper/HangSuppression.h"

#include <cassert>
#include <cstdint>

using skydiag::helper::EvaluateHangSuppression;
using skydiag::helper::HangSuppressionReason;
using skydiag::helper::HangSuppressionState;

static void Test_Suppresses_WhenNotForeground_AndResponsive()
{
  HangSuppressionState s{};
  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,  // responsive → normal Alt-Tab, suppress
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  assert(r.suppress);
  assert(r.reason == HangSuppressionReason::kNotForeground);
  assert(s.suppressedHeartbeatQpc == 200);
  assert(s.foregroundResumeQpc == 0);
}

static void Test_DoesNotSuppress_WhenNotForeground_AndUnresponsive()
{
  // When the window is unresponsive AND not foreground, the game is likely
  // truly frozen (user Alt-Tabbed away from a freeze).  Do NOT suppress.
  HangSuppressionState s{};
  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/false,  // unresponsive → real freeze, allow capture
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  assert(!r.suppress);
  assert(r.reason == HangSuppressionReason::kNone);
  assert(s.suppressedHeartbeatQpc == 0);  // Not set because no suppression
}

static void Test_AltTab_Back_Foreground_Grace()
{
  HangSuppressionState s{};

  // Background hang suppression begins (window responsive = normal Alt-Tab).
  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  // User returns to foreground but the heartbeat has not advanced yet.
  {
    const auto r = EvaluateHangSuppression(
      s,
      /*isHang=*/true,
      /*isForeground=*/true,
      /*isLoading=*/false,
      /*isWindowResponsive=*/false,
      /*suppressHangWhenNotForeground=*/true,
      /*nowQpc=*/1100,
      /*heartbeatQpc=*/200,
      /*qpcFreq=*/100,
      /*foregroundGraceSec=*/5);

    assert(r.suppress);
    assert(r.reason == HangSuppressionReason::kForegroundGrace);
    assert(s.foregroundResumeQpc == 1100);
  }

  // Still within grace (3 seconds).
  {
    const auto r = EvaluateHangSuppression(
      s,
      /*isHang=*/true,
      /*isForeground=*/true,
      /*isLoading=*/false,
      /*isWindowResponsive=*/false,
      /*suppressHangWhenNotForeground=*/true,
      /*nowQpc=*/1400,
      /*heartbeatQpc=*/200,
      /*qpcFreq=*/100,
      /*foregroundGraceSec=*/5);
    assert(r.suppress);
    assert(r.reason == HangSuppressionReason::kForegroundGrace);
  }

  // Past grace -> no suppression anymore (caller may capture hang).
  {
    const auto r = EvaluateHangSuppression(
      s,
      /*isHang=*/true,
      /*isForeground=*/true,
      /*isLoading=*/false,
      /*isWindowResponsive=*/false,
      /*suppressHangWhenNotForeground=*/true,
      /*nowQpc=*/1700,
      /*heartbeatQpc=*/200,
      /*qpcFreq=*/100,
      /*foregroundGraceSec=*/5);
    assert(!r.suppress);
    assert(r.reason == HangSuppressionReason::kNone);
  }
}

static void Test_AltTab_Back_Foreground_StaysSuppressed_WhenResponsive()
{
  HangSuppressionState s{};

  // Background hang suppression begins (window responsive = normal Alt-Tab).
  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  // User returns to foreground but the heartbeat has not advanced yet.
  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1100,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  // Past grace. If the window is still responsive, keep suppressing to avoid false dumps on Alt-Tab.
  {
    const auto r = EvaluateHangSuppression(
      s,
      /*isHang=*/true,
      /*isForeground=*/true,
      /*isLoading=*/false,
      /*isWindowResponsive=*/true,
      /*suppressHangWhenNotForeground=*/true,
      /*nowQpc=*/1700,
      /*heartbeatQpc=*/200,
      /*qpcFreq=*/100,
      /*foregroundGraceSec=*/5);

    assert(r.suppress);
    assert(r.reason == HangSuppressionReason::kForegroundResponsive);
  }
}

static void Test_Clears_WhenHeartbeatAdvances()
{
  HangSuppressionState s{};

  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  // Heartbeat advanced after returning to foreground -> clear suppression.
  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/false,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1100,
    /*heartbeatQpc=*/250,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  assert(!r.suppress);
  assert(s.suppressedHeartbeatQpc == 0);
  assert(s.foregroundResumeQpc == 0);
}

static void Test_NoGraceConfigured_DoesNotSuppressInForeground()
{
  HangSuppressionState s{};

  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/false,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1100,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/0);

  assert(!r.suppress);
  assert(r.reason == HangSuppressionReason::kNone);
}

static void Test_ZeroQpcFreq_DoesNotGetStuckSuppressed()
{
  HangSuppressionState s{};

  (void)EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/false,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/true,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1100,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/0,
    /*foregroundGraceSec=*/5);

  assert(!r.suppress);
  assert(r.reason == HangSuppressionReason::kNone);

  const auto r2 = EvaluateHangSuppression(
    s,
    /*isHang=*/true,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/false,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1200,
    /*heartbeatQpc=*/250,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  assert(!r2.suppress);
  assert(r2.reason == HangSuppressionReason::kNone);
  assert(s.suppressedHeartbeatQpc == 0);
  assert(s.foregroundResumeQpc == 0);
}

static void Test_Resets_WhenNoHang()
{
  HangSuppressionState s{};
  s.suppressedHeartbeatQpc = 200;
  s.foregroundResumeQpc = 123;

  const auto r = EvaluateHangSuppression(
    s,
    /*isHang=*/false,
    /*isForeground=*/true,
    /*isLoading=*/false,
    /*isWindowResponsive=*/false,
    /*suppressHangWhenNotForeground=*/true,
    /*nowQpc=*/1000,
    /*heartbeatQpc=*/200,
    /*qpcFreq=*/100,
    /*foregroundGraceSec=*/5);

  assert(!r.suppress);
  assert(s.suppressedHeartbeatQpc == 0);
  assert(s.foregroundResumeQpc == 0);
}

int main()
{
  Test_Suppresses_WhenNotForeground_AndResponsive();
  Test_DoesNotSuppress_WhenNotForeground_AndUnresponsive();
  Test_AltTab_Back_Foreground_Grace();
  Test_AltTab_Back_Foreground_StaysSuppressed_WhenResponsive();
  Test_Clears_WhenHeartbeatAdvances();
  Test_NoGraceConfigured_DoesNotSuppressInForeground();
  Test_ZeroQpcFreq_DoesNotGetStuckSuppressed();
  Test_Resets_WhenNoHang();
  return 0;
}
