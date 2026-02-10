#include "AnalyzerInternals.h"

#include <algorithm>
#include <filesystem>

#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::minidump::WideLower;

}  // namespace

std::wstring EventTypeName(std::uint16_t t)
{
  using skydiag::EventType;
  switch (static_cast<EventType>(t)) {
    case EventType::kSessionStart: return L"SessionStart";
    case EventType::kHeartbeat: return L"Heartbeat";
    case EventType::kMenuOpen: return L"MenuOpen";
    case EventType::kMenuClose: return L"MenuClose";
    case EventType::kLoadStart: return L"LoadStart";
    case EventType::kLoadEnd: return L"LoadEnd";
    case EventType::kCellChange: return L"CellChange";
    case EventType::kNote: return L"Note";
    case EventType::kPerfHitch: return L"PerfHitch";
    case EventType::kCrash: return L"Crash";
    case EventType::kHangMark: return L"HangMark";
    default: return L"Unknown";
  }
}

std::optional<std::uint32_t> InferMainThreadIdFromEvents(const std::vector<EventRow>& events)
{
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type == static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat) && it->tid != 0) {
      return it->tid;
    }
  }
  return std::nullopt;
}

std::optional<double> InferHeartbeatAgeFromEventsSec(const std::vector<EventRow>& events)
{
  if (events.empty()) {
    return std::nullopt;
  }

  double maxMs = events[0].t_ms;
  double lastHbMs = -1.0;
  for (const auto& e : events) {
    maxMs = std::max(maxMs, e.t_ms);
    if (e.type == static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat)) {
      lastHbMs = std::max(lastHbMs, e.t_ms);
    }
  }
  if (lastHbMs < 0.0) {
    return std::nullopt;
  }

  const double ageMs = std::max(0.0, maxMs - lastHbMs);
  return ageMs / 1000.0;
}

std::wstring ResourceKindFromPath(std::wstring_view path)
{
  const std::filesystem::path p(path);
  const std::wstring ext = WideLower(p.extension().wstring());
  if (ext == L".nif") {
    return L"nif";
  }
  if (ext == L".hkx") {
    return L"hkx";
  }
  if (ext == L".tri") {
    return L"tri";
  }
  if (!ext.empty()) {
    return ext.substr(1);
  }
  return L"(unknown)";
}

}  // namespace skydiag::dump_tool::internal
