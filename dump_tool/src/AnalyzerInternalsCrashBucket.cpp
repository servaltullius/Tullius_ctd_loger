#include "AnalyzerInternals.h"

#include "Bucket.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>

namespace skydiag::dump_tool::internal {
namespace {

// ADR-0009: a freeze is grouped by what the main thread was waiting in. The
// stack below that wait differs between captures of the same freeze (which
// plugins happened to be on the call path, how deep the walk got), so stack
// keys put every capture of one GPU stall in its own bucket. Only freezes have
// a main-thread wait or a modal dialog, so crashes keep the stack key.
std::optional<std::string> HangWaitCanonical(const AnalysisResult& out)
{
  const auto lower = [](std::wstring_view text) {
    return bucket::NarrowAsciiFallback(bucket::LowerTrimmed(text));
  };
  std::ostringstream canonical;
  canonical << "v=1|";
  if (out.modal_dialog_wait.detected) {
    canonical << "kind=modal_dialog|caller=" << lower(out.modal_dialog_wait.caller_module_filename)
              << "|addrlib=" << out.modal_dialog_wait.address_library_issue;
    return canonical.str();
  }
  const auto& wait = out.main_thread_wait;
  if (wait.kind == "engine_wait" || wait.kind == "graphics_driver_wait" || wait.kind == "plugin_wait") {
    canonical << "kind=" << wait.kind << "|detail=" << wait.engine_wait_detail << "|module=" << lower(wait.waiting_module)
              << "|api=" << lower(wait.wait_api);
    // The main thread waits the same way for every game thread; which plugin
    // held the thread it waited for tells these freezes apart.
    if (wait.blocked_thread_tid != 0u) {
      canonical << "|blocked=" << lower(wait.blocked_module) << "|state=" << wait.blocked_state;
    }
    return canonical.str();
  }
  return std::nullopt;  // running or unknown: the stack is all there is
}

// lock_owner_wait and crash_logger_thread_dump_deadlock: the main thread waits
// the same way in all of them (often in the graphics driver), so its wait
// would put them in the bucket of every ordinary driver wait. The deadlock is
// one cause whichever thread it suspended; a lock-owner wait is told apart by
// the owner thread, and keeps the wait key when nothing names that thread.
std::optional<std::wstring> LockOwnerFreezeBucketKey(const AnalysisResult& out)
{
  const auto& state = out.freeze_analysis.state_id;
  if (!IsLockOwnerFreezeState(state) || out.main_thread_lock_owner.tid == 0u) {
    return std::nullopt;
  }
  if (state == "crash_logger_thread_dump_deadlock") {
    return ComputeFreezeStateBucketKey(state);
  }
  const auto& owner = out.main_thread_lock_owner;
  if (owner.top_module.empty() && owner.thread_name.empty()) {
    return std::nullopt;
  }
  return ComputeLockOwnerBucketKey(state, owner.top_module, owner.thread_name);
}

}  // namespace

void ComputeCrashBucket(AnalysisResult& out)
{
  if (auto key = LockOwnerFreezeBucketKey(out)) {
    out.crash_bucket_version = 3;
    out.crash_bucket_key = std::move(*key);
    return;
  }
  if (const auto canonical = HangWaitCanonical(out)) {
    out.crash_bucket_version = 3;
    out.crash_bucket_key = ComputeHangBucketKey(*canonical);
    return;
  }

  // Avoid generating a misleading "always the same" bucket key for snapshot/manual dumps that
  // contain no exception/module/callstack information.
  if (out.exc_code == 0u &&
      out.stackwalk_primary_frames.empty() &&
      out.suspects.empty() &&
      out.fault_module_plus_offset.empty()) {
    out.crash_bucket_key.clear();
    return;
  }

  // ADR-0016: the stub address and the walk from it change with every run;
  // whose hook it was and where it jumps do not.
  if (out.hook_stub_race.detected) {
    out.crash_bucket_version = 3;
    out.crash_bucket_key = ComputeCrashBucketKey(
      out.exc_code,
      L"hook_stub:" + out.hook_stub_race.target_module,
      out.hook_stub_race.target_offset,
      {});
    return;
  }

  std::vector<CrashBucketFrame> bucketFrames;
  if (!out.stackwalk_primary_bucket_frames.empty()) {
    const std::size_t n = std::min<std::size_t>(out.stackwalk_primary_bucket_frames.size(), 6);
    bucketFrames.assign(out.stackwalk_primary_bucket_frames.begin(), out.stackwalk_primary_bucket_frames.begin() + n);
  } else if (!out.suspects.empty()) {
    const std::size_t n = std::min<std::size_t>(out.suspects.size(), 4);
    bucketFrames.reserve(n);
    for (std::size_t i = 0; i < n; i++) {
      bucketFrames.push_back({ out.suspects[i].module_filename, 0 });
    }
  }

  std::wstring faultModule = out.fault_module_filename;
  if (faultModule.empty()) {
    faultModule = out.fault_module_plus_offset;
  }
  // v3: the formal stackwalk unwinds real caller frames instead of stopping at
  // the faulting frame, so v2 keys would not group the same incidents.
  out.crash_bucket_version = 3;
  out.crash_bucket_key = ComputeCrashBucketKey(
    out.exc_code,
    faultModule,
    out.fault_module_offset,
    bucketFrames);
}

bool RekeyLockOwnerFreezeBucket(AnalysisResult& out)
{
  const auto key = LockOwnerFreezeBucketKey(out);
  if (!key || *key == out.crash_bucket_key) {
    return false;
  }
  ComputeCrashBucket(out);
  return true;
}

}  // namespace skydiag::dump_tool::internal
