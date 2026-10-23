#include "GameLockWait.h"

#include <algorithm>
#include <cwchar>
#include <optional>
#include <unordered_map>

#include "MainThreadWait.h"

namespace skydiag::dump_tool {
namespace {

using skydiag::game_locks::DecodeHeldLock;
using skydiag::game_locks::HeldKind;
using skydiag::game_locks::PointsIntoWritable;

// The frame that called the sleep: the first one that is neither Windows nor
// the graphics stack (as ClassifyMainThreadWait finds it).
std::optional<std::size_t> WaitSiteIndex(const std::vector<ModalStackFrame>& frames)
{
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module) {
      return std::nullopt;
    }
    if (!frame.is_system && !IsGraphicsStackModule(frame.module_filename)) {
      return frame.is_game_exe ? std::optional<std::size_t>(i) : std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace

std::wstring GameFrameText(const GameFrameRef& frame)
{
  wchar_t offset[32]{};
  std::swprintf(offset, sizeof(offset) / sizeof(offset[0]), L"+0x%llx", static_cast<unsigned long long>(frame.offset));
  return (frame.module.empty() ? std::wstring(L"?") : frame.module) + offset;
}

std::vector<std::uint32_t> EngineLockDeadlockThreads(const EngineLockDeadlockInfo& deadlock)
{
  if (deadlock.cycle.empty()) {
    return deadlock.pattern_tids;
  }
  std::vector<std::uint32_t> tids;
  tids.reserve(deadlock.cycle.size());
  for (const auto& link : deadlock.cycle) {
    tids.push_back(link.tid);
  }
  return tids;
}

bool IsReportedGameLockWait(const GameLockWaitThread& wait, const EngineLockDeadlockInfo& deadlock)
{
  if (!wait.candidates.empty()) {
    return true;
  }
  if (!deadlock.detected) {
    return false;
  }
  const auto tids = EngineLockDeadlockThreads(deadlock);
  return std::find(tids.begin(), tids.end(), wait.tid) != tids.end();
}

std::wstring JoinThreadIds(const std::vector<std::uint32_t>& tids)
{
  std::wstring out;
  for (const auto tid : tids) {
    out += (out.empty() ? L"" : L", ") + std::to_wstring(tid);
  }
  return out;
}

std::wstring DescribeEngineLockKind(const std::string& kind, bool en)
{
  if (kind == "rw_write") {
    return en ? L"read/write lock held for write" : L"쓰기로 잡힌 읽기/쓰기 잠금";
  }
  return en ? L"spin lock" : L"스핀 잠금";
}

std::wstring DescribeEngineLockCycle(const EngineLockDeadlockInfo& deadlock, bool en)
{
  std::wstring out;
  for (const auto& link : deadlock.cycle) {
    const std::wstring lock = GameFrameText(GameFrameRef{ deadlock.module, link.lock_rva });
    const std::wstring kind = DescribeEngineLockKind(link.kind, en);
    if (!out.empty()) {
      out += en ? L"; " : L", ";
    }
    out += en
      ? (L"thread " + std::to_wstring(link.tid) + L" waited for " + lock + L" (" + kind + L"), held by thread " +
          std::to_wstring(link.owner_tid))
      : (L"스레드 " + std::to_wstring(link.tid) + L"은(는) 스레드 " + std::to_wstring(link.owner_tid) + L"이(가) 쥔 " +
          lock + L"(" + kind + L")을 기다림");
  }
  return out;
}

std::vector<GameLockWaitThread> FindGameLockWaits(
  const std::vector<GameLockThreadInput>& threads,
  const GameLockImage& image,
  const DumpMemoryReader& read,
  const std::vector<std::uint32_t>& dumpThreadIds)
{
  std::vector<GameLockWaitThread> waits;
  for (const auto& thread : threads) {
    const auto wait = ClassifyMainThreadWait(thread.top_frames);
    if (wait.kind != "engine_wait" || wait.wait_class != "sleep") {
      continue;
    }
    const auto site = WaitSiteIndex(thread.top_frames);
    if (!site || *site >= thread.frames.size() || thread.frames[*site].module.empty()) {
      continue;
    }
    GameLockWaitThread entry{};
    entry.tid = thread.tid;
    entry.is_main_thread = thread.is_main_thread;
    const std::size_t end = std::min(thread.frames.size(), *site + kGameLockFrames);
    entry.frames.assign(thread.frames.begin() + static_cast<std::ptrdiff_t>(*site),
      thread.frames.begin() + static_cast<std::ptrdiff_t>(end));
    entry.wait_module = entry.frames.front().module;
    entry.wait_offset = entry.frames.front().offset;

    if (!image.writable.empty() && read) {
      for (const auto& [location, value] : thread.roots) {
        if (entry.candidates.size() >= kGameLockMaxCandidates) {
          break;
        }
        if (!PointsIntoWritable(image.writable, value)) {
          continue;
        }
        const std::uint64_t rva = value - image.base;
        if (std::any_of(entry.candidates.begin(), entry.candidates.end(),
              [rva](const GameLockCandidate& c) { return c.rva == rva; })) {
          continue;
        }
        std::uint32_t words[2]{};
        if (!read(value, words, sizeof(words))) {
          continue;  // the dump does not hold it
        }
        const auto held = DecodeHeldLock(words[0], words[1]);
        if (held.kind == HeldKind::kNone || held.owner == thread.tid ||
            std::find(dumpThreadIds.begin(), dumpThreadIds.end(), held.owner) == dumpThreadIds.end()) {
          continue;
        }
        GameLockCandidate candidate{};
        candidate.location = location;
        candidate.rva = rva;
        candidate.kind = held.kind == HeldKind::kSpin ? "spin" : "rw_write";
        candidate.owner_tid = held.owner;
        candidate.count = held.count;
        entry.candidates.push_back(std::move(candidate));
      }
    }
    waits.push_back(std::move(entry));
  }
  for (auto& entry : waits) {
    for (auto& candidate : entry.candidates) {
      candidate.owner_waiting = std::any_of(waits.begin(), waits.end(),
        [&](const GameLockWaitThread& other) { return other.tid == candidate.owner_tid; });
    }
  }
  return waits;
}

std::vector<EngineLockDeadlockInfo::Link> FindEngineLockCycle(const std::vector<GameLockWaitThread>& waits)
{
  std::unordered_map<std::uint32_t, std::size_t> indexByTid;
  for (std::size_t i = 0; i < waits.size(); ++i) {
    indexByTid.emplace(waits[i].tid, i);
  }
  // Depth-first from each thread in order, along the first candidate (in the
  // helper's order) whose owner is waiting too; each step's choices are
  // tried in order, so the result does not depend on hash order.
  for (std::size_t start = 0; start < waits.size(); ++start) {
    std::vector<EngineLockDeadlockInfo::Link> path;
    std::vector<std::size_t> onPath;
    std::vector<std::size_t> cursor;
    std::size_t current = start;
    cursor.push_back(0);
    onPath.push_back(current);
    while (!cursor.empty()) {
      const auto& candidates = waits[current].candidates;
      std::size_t& next = cursor.back();
      bool advanced = false;
      while (next < candidates.size()) {
        const auto& candidate = candidates[next++];
        if (!candidate.owner_waiting) {
          continue;
        }
        const auto it = indexByTid.find(candidate.owner_tid);
        if (it == indexByTid.end()) {
          continue;
        }
        EngineLockDeadlockInfo::Link link{};
        link.tid = waits[current].tid;
        link.lock_rva = candidate.rva;
        link.kind = candidate.kind;
        link.owner_tid = candidate.owner_tid;
        if (it->second == start) {
          path.push_back(std::move(link));
          return path;
        }
        if (std::find(onPath.begin(), onPath.end(), it->second) != onPath.end()) {
          continue;  // a cycle that does not pass through start: found from its own start
        }
        path.push_back(std::move(link));
        current = it->second;
        onPath.push_back(current);
        cursor.push_back(0);
        advanced = true;
        break;
      }
      if (advanced) {
        continue;
      }
      cursor.pop_back();
      onPath.pop_back();
      if (!path.empty()) {
        path.pop_back();
      }
      if (!onPath.empty()) {
        current = onPath.back();
      }
    }
  }
  return {};
}

}  // namespace skydiag::dump_tool
