#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "MinidumpUtil.h"
#include "SkyrimDiagCloseRequest.h"
#include "SkyrimDiagSaveLoad.h"
#include "SkyrimDiagShared.h"
#include "Utf.h"

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
         !minidump::IsExternalSoftwareModule(moduleName) &&
         !minidump::IsGameExeModule(moduleName);
}

// Vanilla menus a player opens on purpose. HUD widgets and overlays (Cursor
// Menu, TrueHUD, casting bars, ...) stay open all session and say nothing.
constexpr std::wstring_view kGameMenus[] = {
  L"Console", L"TweenMenu", L"InventoryMenu", L"MagicMenu", L"MapMenu", L"Journal Menu", L"ContainerMenu",
  L"BarterMenu", L"GiftMenu", L"Book Menu", L"Dialogue Menu", L"Crafting Menu", L"Lockpicking Menu",
  L"Sleep/Wait Menu", L"StatsMenu", L"FavoritesMenu", L"LevelUp Menu", L"Training Menu", L"MessageBoxMenu",
  L"Main Menu", L"Loading Menu", L"RaceSex Menu", L"Tutorial Menu", L"Creation Club Menu", L"Mod Manager Menu",
  L"Credits Menu",
};

bool IsGameMenuName(const std::wstring& name)
{
  return std::find(std::begin(kGameMenus), std::end(kGameMenus), name) != std::end(kGameMenus);
}

// A main-thread gap this long is the PC sleeping or the game minimized, not a
// stutter the player sat through.
constexpr double kPauseGapMs = 300000.0;

void AddGameStateContext(const std::vector<EventRow>& events, double lastMs, BlackboxFreezeSummary& summary)
{
  std::vector<std::pair<std::wstring, double>> open;  // menu, opened at
  double lastLoadEndMs = -1.0;
  for (const auto& event : events) {
    switch (static_cast<skydiag::EventType>(event.type)) {
      case skydiag::EventType::kMenuOpen: {
        const auto name = DecodePackedShortText(event);
        if (IsGameMenuName(name) &&
            std::none_of(open.begin(), open.end(), [&](const auto& entry) { return entry.first == name; })) {
          open.emplace_back(name, event.t_ms);
        }
        break;
      }
      case skydiag::EventType::kMenuClose: {
        const auto name = DecodePackedShortText(event);
        open.erase(
          std::remove_if(open.begin(), open.end(), [&](const auto& entry) { return entry.first == name; }),
          open.end());
        break;
      }
      case skydiag::EventType::kLoadEnd:
        lastLoadEndMs = std::max(lastLoadEndMs, event.t_ms);
        break;
      case skydiag::EventType::kPerfHitch: {
        const auto gapMs = static_cast<double>(event.a);
        if (gapMs >= kPauseGapMs && gapMs / 1000.0 > summary.pause_gap_seconds) {
          summary.pause_gap_seconds = gapMs / 1000.0;
          summary.pause_gap_ended_seconds_before = std::max(0.0, (lastMs - event.t_ms) / 1000.0);
        }
        break;
      }
      default:
        break;
    }
  }
  for (const auto& [name, _] : open) {
    summary.open_menus.push_back(name);
  }
  if (lastLoadEndMs >= 0.0) {
    summary.seconds_since_load_end = std::max(0.0, (lastMs - lastLoadEndMs) / 1000.0);
  }
}

}  // namespace

ForcedCloseInfo DetectForcedClose(const std::vector<EventRow>& events)
{
  skydiag::ForcedCloseScanner scanner;
  for (const auto& event : events) {
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCloseRequest)) {
      scanner.AddCloseRequest(event.t_ms, event.a);
    } else if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash)) {
      scanner.AddCrash(event.t_ms);
    }
  }
  const auto match = scanner.Result();
  ForcedCloseInfo info{};
  if (match.matched()) {
    info.detected = true;
    info.kind = skydiag::CloseRequestKindId(match.kind);
    info.seconds_before_crash = match.msBeforeCrash / 1000.0;
  }
  return info;
}

std::wstring DecodeSaveLoadName(std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
  char buf[sizeof(b) * 3]{};
  std::memcpy(buf, &b, sizeof(b));
  std::memcpy(buf + sizeof(b), &c, sizeof(c));
  std::memcpy(buf + sizeof(b) * 2, &d, sizeof(d));
  return Utf8OrAnsiToWide(std::string_view(buf, strnlen(buf, skydiag::kSaveNameTextBytes)));
}

SaveLoadInfo DetectSaveLoad(const std::vector<EventRow>& events)
{
  SaveLoadInfo info{};
  double startedMs = 0.0;
  double lastMs = 0.0;
  double crashMs = -1.0;
  for (const auto& event : events) {
    lastMs = std::max(lastMs, event.t_ms);
    switch (static_cast<skydiag::EventType>(event.type)) {
      case skydiag::EventType::kSaveLoadStart: {
        info = SaveLoadInfo{};
        info.seen = true;
        info.in_progress = true;
        info.name = DecodeSaveLoadName(event.b, event.c, event.d);
        if (event.a != 0u) {
          char key[17]{};
          std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(event.a));
          info.key = key;
        }
        startedMs = event.t_ms;
        crashMs = -1.0;
        break;
      }
      case skydiag::EventType::kSaveLoadEnd:
        if (event.a == static_cast<std::uint64_t>(skydiag::SaveLoadResult::kNewGame)) {
          info = SaveLoadInfo{};  // a new game: no save is in play
        } else if (info.seen && event.t_ms >= startedMs) {
          info.in_progress = false;
        }
        break;
      case skydiag::EventType::kCrash:
        if (info.in_progress && event.t_ms >= startedMs) {
          crashMs = event.t_ms;
        }
        break;
      default:
        break;
    }
  }
  if (info.in_progress) {
    info.timed_to_crash = crashMs >= 0.0;
    info.seconds_loading = std::max(0.0, ((info.timed_to_crash ? crashMs : lastMs) - startedMs) / 1000.0);
  }
  return info;
}

std::string SaveLoadFailurePlace(const AnalysisResult& r)
{
  if (r.exc_code == 0u) {
    return "freeze|" + WideToUtf8(r.crash_bucket_key);
  }
  char code[16]{};
  std::snprintf(code, sizeof(code), "0x%08x", r.exc_code);
  if (!r.fault_module_filename.empty()) {
    char offset[24]{};
    std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(r.fault_module_offset));
    return std::string(code) + "|" + WideToUtf8(minidump::WideLower(r.fault_module_filename)) + offset;
  }
  const auto& fault = r.fault_instruction;
  return std::string(code) + "|" + fault.verdict + "|" + fault.access + "|" + fault.target_state;
}

bool CountsAsFailedLoad(const AnalysisResult& r)
{
  return r.save_load.in_progress && !r.save_load.key.empty() && !r.is_filtered_clean_exit &&
         !r.forced_close.detected && (r.is_crash_like || (r.is_hang_like && !r.is_manual_capture));
}

BlackboxFreezeSummary BuildBlackboxFreezeSummary(
  const std::vector<EventRow>& events,
  bool loadingContext,
  double lastHeartbeatMs)
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

  // Quiet play writes no events, so the newest one can be long before the
  // freeze. The main thread's last heartbeat is when it stopped.
  AddGameStateContext(events, std::max(maxMs, lastHeartbeatMs), summary);

  summary.has_context =
    summary.recent_module_loads > 0u ||
    summary.recent_module_unloads > 0u ||
    summary.recent_thread_creates > 0u ||
    summary.recent_thread_exits > 0u;
  return summary;
}

}  // namespace skydiag::dump_tool::internal
