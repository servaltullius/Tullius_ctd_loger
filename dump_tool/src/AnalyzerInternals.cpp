#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string_view>

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

namespace {

constexpr std::uint64_t Fnv1a64(std::string_view s) noexcept
{
  std::uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

struct MenuHashEntry {
  std::uint64_t hash;
  const wchar_t* name;
};

// Compile-time FNV-1a 64-bit hashes of known Skyrim menu names.
// Source: RE::*Menu::MENU_NAME from CommonLibSSE-NG
constexpr MenuHashEntry kKnownMenuHashes[] = {
  {Fnv1a64(""), L"(empty)"},
  // Core UI
  {Fnv1a64("Console"), L"Console"},
  {Fnv1a64("Console Native UI Menu"), L"Console Native UI Menu"},
  {Fnv1a64("Loading Menu"), L"Loading Menu"},
  {Fnv1a64("Main Menu"), L"Main Menu"},
  {Fnv1a64("Mist Menu"), L"Mist Menu"},
  {Fnv1a64("Fader Menu"), L"Fader Menu"},
  {Fnv1a64("Cursor Menu"), L"Cursor Menu"},
  {Fnv1a64("HUD Menu"), L"HUD Menu"},
  {Fnv1a64("Credits Menu"), L"Credits Menu"},
  {Fnv1a64("Creation Club Menu"), L"Creation Club Menu"},
  // Gameplay
  {Fnv1a64("Dialogue Menu"), L"Dialogue Menu"},
  {Fnv1a64("InventoryMenu"), L"InventoryMenu"},
  {Fnv1a64("MagicMenu"), L"MagicMenu"},
  {Fnv1a64("MapMenu"), L"MapMenu"},
  {Fnv1a64("Sleep/Wait Menu"), L"Sleep/Wait Menu"},
  {Fnv1a64("ContainerMenu"), L"ContainerMenu"},
  {Fnv1a64("BarterMenu"), L"BarterMenu"},
  {Fnv1a64("GiftMenu"), L"GiftMenu"},
  {Fnv1a64("Lockpicking Menu"), L"Lockpicking Menu"},
  {Fnv1a64("Book Menu"), L"Book Menu"},
  {Fnv1a64("Journal Menu"), L"Journal Menu"},
  {Fnv1a64("MessageBoxMenu"), L"MessageBoxMenu"},
  {Fnv1a64("Crafting Menu"), L"Crafting Menu"},
  {Fnv1a64("Training Menu"), L"Training Menu"},
  {Fnv1a64("Tutorial Menu"), L"Tutorial Menu"},
  {Fnv1a64("TweenMenu"), L"TweenMenu"},
  {Fnv1a64("StatsMenu"), L"StatsMenu"},
  {Fnv1a64("LevelUp Menu"), L"LevelUp Menu"},
  {Fnv1a64("FavoritesMenu"), L"FavoritesMenu"},
  {Fnv1a64("Kinect Menu"), L"Kinect Menu"},
  // Character / Mod menus
  {Fnv1a64("RaceSex Menu"), L"RaceSex Menu"},
  {Fnv1a64("CustomMenu"), L"CustomMenu"},
};

const wchar_t* LookupMenuName(std::uint64_t hash)
{
  for (const auto& entry : kKnownMenuHashes) {
    if (entry.hash == hash) {
      return entry.name;
    }
  }
  return nullptr;
}

std::wstring DecodeStateFlags(std::uint64_t flags)
{
  if (flags == 0) return L"None";
  std::wstring result;
  if (flags & skydiag::kState_Frozen) {
    result += L"Frozen";
  }
  if (flags & skydiag::kState_Loading) {
    if (!result.empty()) result += L"|";
    result += L"Loading";
  }
  if (flags & skydiag::kState_InMenu) {
    if (!result.empty()) result += L"|";
    result += L"InMenu";
  }
  return result.empty() ? (L"0x" + std::to_wstring(flags)) : result;
}

}  // anonymous namespace

std::wstring FormatEventDetail(std::uint16_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
  using skydiag::EventType;
  switch (static_cast<EventType>(type)) {
    case EventType::kPerfHitch: {
      // a=hitch ms, b=state_flags, c=heartbeat interval ms
      std::wstring s = L"hitch=";
      if (a >= 1000) {
        s += std::to_wstring(a / 1000) + L"." + std::to_wstring((a % 1000) / 100) + L"s";
      } else {
        s += std::to_wstring(a) + L"ms";
      }
      s += L" flags=" + DecodeStateFlags(b);
      s += L" interval=" + std::to_wstring(c) + L"ms";
      return s;
    }

    case EventType::kMenuOpen:
    case EventType::kMenuClose: {
      // Try embedded menu name string first (b+c+d = 24 bytes UTF-8)
      char menuBuf[24]{};
      std::memcpy(menuBuf, &b, 8);
      std::memcpy(menuBuf + 8, &c, 8);
      std::memcpy(menuBuf + 16, &d, 8);
      menuBuf[23] = '\0';  // safety null
      if (menuBuf[0] != '\0') {
        const std::string_view sv(menuBuf);
        std::wstring ws;
        ws.reserve(sv.size());
        for (char ch : sv) {
          ws += static_cast<wchar_t>(static_cast<unsigned char>(ch));
        }
        return ws;
      }

      // Fallback: hash lookup (for old dumps without embedded name)
      const wchar_t* name = LookupMenuName(a);
      if (name) {
        return name;
      }
      wchar_t hexBuf[32];
      std::swprintf(hexBuf, sizeof(hexBuf) / sizeof(hexBuf[0]), L"hash=0x%016llX", static_cast<unsigned long long>(a));
      return hexBuf;
    }

    case EventType::kHeartbeat: {
      // a=state_flags
      return L"flags=" + DecodeStateFlags(a);
    }

    case EventType::kCellChange: {
      if (a != 0) {
        wchar_t buf[32];
        std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"cellId=0x%08X", static_cast<unsigned>(a & 0xFFFFFFFFu));
        return buf;
      }
      return {};
    }

    default:
      return {};
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
