#include "SkyrimDiag/EventSinks.h"

#include <Windows.h>

#include <cstring>
#include <string_view>

#include <RE/Skyrim.h>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiag/Hash.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
  RE::BSEventNotifyControl ProcessEvent(
    const RE::MenuOpenCloseEvent* e,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
  {
    if (!e) {
      return RE::BSEventNotifyControl::kContinue;
    }

    auto* shm = GetShared();
    if (!shm) {
      return RE::BSEventNotifyControl::kContinue;
    }

    const std::string_view menuName = e->menuName.c_str() ? e->menuName.c_str() : "";
    const auto menuHash = skydiag::hash::Fnv1a64(menuName);

    skydiag::EventPayload p{};
    p.a = menuHash;

    // Pack menu name UTF-8 into b+c+d (24 bytes, null-terminated, truncated if longer)
    static_assert(sizeof(p.b) + sizeof(p.c) + sizeof(p.d) == 24);
    constexpr std::size_t kMenuNameMaxBytes = 24;
    char* dst = reinterpret_cast<char*>(&p.b);
    const std::size_t len = menuName.size();
    if (len > 0) {
      const std::size_t copyLen = (len < kMenuNameMaxBytes) ? len : (kMenuNameMaxBytes - 1);
      std::memcpy(dst, menuName.data(), copyLen);
      dst[copyLen] = '\0';
    }

    if (e->opening) {
      PushEvent(skydiag::EventType::kMenuOpen, p, sizeof(p));
      InterlockedOr(
        reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
        static_cast<LONG>(skydiag::kState_InMenu));

      if (menuName == RE::LoadingMenu::MENU_NAME) {
        PushEvent(skydiag::EventType::kLoadStart, p, sizeof(p));
        InterlockedOr(
          reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
          static_cast<LONG>(skydiag::kState_Loading));
      }
    } else {
      PushEvent(skydiag::EventType::kMenuClose, p, sizeof(p));

      // Best-effort flag clearing (UI state can be slightly out-of-date during teardown).
      auto* ui = RE::UI::GetSingleton();
      if (ui && !ui->IsShowingMenus()) {
        InterlockedAnd(
          reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
          ~static_cast<LONG>(skydiag::kState_InMenu));
      }

      if (menuName == RE::LoadingMenu::MENU_NAME) {
        PushEvent(skydiag::EventType::kLoadEnd, p, sizeof(p));
        InterlockedAnd(
          reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
          ~static_cast<LONG>(skydiag::kState_Loading));
      }
    }

    return RE::BSEventNotifyControl::kContinue;
  }
};

MenuSink g_menuSink;

}  // namespace

bool RegisterEventSinks(bool logMenus)
{
  if (!logMenus) {
    return true;
  }

  auto* ui = RE::UI::GetSingleton();
  if (!ui) {
    return false;
  }

  ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
  return true;
}

}  // namespace skydiag::plugin
