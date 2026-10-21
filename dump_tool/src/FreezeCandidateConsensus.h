#pragma once

#include "Analyzer.h"
#include "WctTypes.h"

#include <optional>

namespace skydiag::dump_tool {

struct FreezeSignalInput
{
  bool is_hang_like = false;
  bool is_snapshot_like = false;
  bool is_manual_capture = false;
  bool loading_context = false;
  std::optional<internal::WctFreezeSummary> wct;
  std::optional<BlackboxFreezeSummary> blackbox;
  std::optional<FirstChanceSummary> first_chance;
  std::optional<HangThreadModuleConsensus> thread_module_consensus;
  std::optional<ModalDialogWaitInfo> modal_dialog_wait;
  std::optional<MainThreadWaitInfo> main_thread_wait;
  // The owner of the lock the main thread waits on (tid != 0), and the
  // Crash Logger thread-dump deadlock read from it.
  std::optional<MainThreadLockOwnerInfo> lock_owner;
  std::optional<CrashLoggerThreadDumpDeadlockInfo> crash_logger_thread_dump_deadlock;
  std::vector<ActionableCandidate> actionable_candidates;
};

FreezeAnalysisResult BuildFreezeCandidateConsensus(const FreezeSignalInput& input, i18n::Language language);

}  // namespace skydiag::dump_tool
