#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {
namespace {

std::wstring DecodePackedShortText(const EventRow& event)
{
  char buf[24]{};
  std::memcpy(buf, &event.b, sizeof(event.b));
  std::memcpy(buf + sizeof(event.b), &event.c, sizeof(event.c));
  std::memcpy(buf + sizeof(event.b) + sizeof(event.c), &event.d, sizeof(event.d));
  buf[23] = '\0';

  std::size_t len = 0;
  while (len < sizeof(buf) && buf[len] != '\0') {
    ++len;
  }
  if (len == 0) {
    return {};
  }

  std::wstring out;
  out.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(buf[i])));
  }
  return out;
}

bool IsActionableModuleName(const std::wstring& moduleName)
{
  return !moduleName.empty() &&
         !minidump::IsSystemishModule(moduleName) &&
         !minidump::IsGameExeModule(moduleName);
}

}  // namespace

BlackboxFreezeSummary BuildBlackboxFreezeSummary(
  const std::vector<EventRow>& events,
  bool loadingContext)
{
  BlackboxFreezeSummary summary{};
  summary.loading_window = loadingContext;
  if (events.empty()) {
    return summary;
  }

  double maxMs = 0.0;
  for (const auto& event : events) {
    maxMs = std::max(maxMs, event.t_ms);
  }
  const double windowMs = loadingContext ? 8000.0 : 5000.0;
  const double cutoffMs = std::max(0.0, maxMs - windowMs);

  std::unordered_map<std::wstring, std::uint32_t> moduleHits;
  for (const auto& event : events) {
    if (event.t_ms < cutoffMs) {
      continue;
    }

    switch (static_cast<skydiag::EventType>(event.type)) {
      case skydiag::EventType::kModuleLoad: {
        ++summary.recent_module_loads;
        const auto moduleName = DecodePackedShortText(event);
        if (IsActionableModuleName(moduleName)) {
          ++moduleHits[moduleName];
        }
        break;
      }
      case skydiag::EventType::kModuleUnload: {
        ++summary.recent_module_unloads;
        const auto moduleName = DecodePackedShortText(event);
        if (IsActionableModuleName(moduleName)) {
          ++moduleHits[moduleName];
        }
        break;
      }
      case skydiag::EventType::kThreadCreate:
        ++summary.recent_thread_creates;
        break;
      case skydiag::EventType::kThreadExit:
        ++summary.recent_thread_exits;
        break;
      default:
        break;
    }
  }

  summary.module_churn_score = summary.recent_module_loads + summary.recent_module_unloads;
  summary.thread_churn_score = summary.recent_thread_creates + summary.recent_thread_exits;
  if (!moduleHits.empty() && loadingContext) {
    ++summary.module_churn_score;
  }

  std::vector<std::pair<std::wstring, std::uint32_t>> rankedModules(moduleHits.begin(), moduleHits.end());
  std::sort(rankedModules.begin(), rankedModules.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });
  for (const auto& [moduleName, _] : rankedModules) {
    summary.recent_non_system_modules.push_back(moduleName);
    if (summary.recent_non_system_modules.size() >= 4u) {
      break;
    }
  }

  summary.has_context =
    summary.recent_module_loads > 0u ||
    summary.recent_module_unloads > 0u ||
    summary.recent_thread_creates > 0u ||
    summary.recent_thread_exits > 0u;
  return summary;
}

}  // namespace skydiag::dump_tool::internal
