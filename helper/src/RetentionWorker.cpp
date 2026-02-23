#include "RetentionWorker.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace skydiag::helper::internal {
namespace {

struct RetentionTask
{
  std::filesystem::path outBase;
  skydiag::helper::RetentionLimits limits{};
};

struct RetentionWorkerState
{
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::wstring, RetentionTask> pending;
  std::thread worker;
  bool started = false;
  bool stop = false;
};

RetentionWorkerState& GetRetentionWorkerState()
{
  static RetentionWorkerState state{};
  return state;
}

std::wstring MakePathKey(const std::filesystem::path& path)
{
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec && !canonical.empty()) {
    return canonical.wstring();
  }
  return path.lexically_normal().wstring();
}

void RetentionWorkerMain()
{
  auto& state = GetRetentionWorkerState();

  for (;;) {
    std::unordered_map<std::wstring, RetentionTask> batch;
    {
      std::unique_lock lock(state.mutex);
      state.cv.wait(lock, [&state]() {
        return state.stop || !state.pending.empty();
      });
      if (state.stop && state.pending.empty()) {
        break;
      }
      batch.swap(state.pending);
    }

    for (const auto& [_, task] : batch) {
      skydiag::helper::ApplyRetentionToOutputDir(task.outBase, task.limits);
    }
  }
}

void EnsureWorkerStartedLocked(RetentionWorkerState& state)
{
  if (state.started) {
    return;
  }
  state.stop = false;
  state.worker = std::thread(RetentionWorkerMain);
  state.started = true;
}

}  // namespace

void QueueRetentionSweep(const std::filesystem::path& outBase, const skydiag::helper::RetentionLimits& limits)
{
  if (outBase.empty()) {
    return;
  }

  auto& state = GetRetentionWorkerState();
  {
    std::lock_guard lock(state.mutex);
    EnsureWorkerStartedLocked(state);
    const auto key = MakePathKey(outBase);
    state.pending[key] = RetentionTask{ outBase, limits };
  }
  state.cv.notify_one();
}

void ShutdownRetentionWorker()
{
  auto& state = GetRetentionWorkerState();

  std::thread worker;
  {
    std::lock_guard lock(state.mutex);
    if (!state.started) {
      return;
    }
    state.stop = true;
    state.cv.notify_one();
    worker = std::move(state.worker);
    state.started = false;
  }

  if (worker.joinable()) {
    worker.join();
  }

  {
    std::lock_guard lock(state.mutex);
    state.pending.clear();
    state.stop = false;
  }
}

}  // namespace skydiag::helper::internal
