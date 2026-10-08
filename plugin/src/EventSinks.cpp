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

// The helper picks its longer menu hang threshold from kState_InMenu, so the
// flag means "a menu has taken over the game", not "some menu is open":
// HUD-style menus (HUD Menu, Cursor Menu, TrueHUD, widget menus) stay open
// throughout gameplay. UI::IsShowingMenus() only reports HUD visibility.
bool MenuTakesOverGame(const RE::IMenu& menu) noexcept
{
  return menu.PausesGame() || menu.Modal() || menu.ApplicationMenu() || menu.InventoryItemMenu();
}

void StoreInMenuFlag(skydiag::SharedLayout& shm, RE::UI& ui, bool openingMenuTakesOverGame) noexcept
{
  const bool inMenu = openingMenuTakesOverGame || ui.GameIsPaused() || ui.IsModalMenuOpen() ||
                      ui.IsApplicationMenuOpen() || ui.IsItemMenuOpen();
  auto* flags = reinterpret_cast<volatile LONG*>(&shm.header.state_flags);
  if (inMenu) {
    InterlockedOr(flags, static_cast<LONG>(skydiag::kState_InMenu));
  } else if (!ui.closingAllMenus) {
    // While the game closes every menu at once (save load, quit) the counters
    // are in flux; keep the flag until the next refresh.
    InterlockedAnd(flags, ~static_cast<LONG>(skydiag::kState_InMenu));
  }
}

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

    auto* ui = RE::UI::GetSingleton();
    if (e->opening) {
      PushEvent(skydiag::EventType::kMenuOpen, p, sizeof(p));
      if (ui) {
        // The UI counters may not include the menu being opened yet.
        const auto menu = ui->GetMenu(menuName);
        StoreInMenuFlag(*shm, *ui, menu && MenuTakesOverGame(*menu));
      } else {
        InterlockedOr(
          reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
          static_cast<LONG>(skydiag::kState_InMenu));
      }

      if (menuName == RE::LoadingMenu::MENU_NAME) {
        PushEvent(skydiag::EventType::kLoadStart, p, sizeof(p));
        InterlockedOr(
          reinterpret_cast<volatile LONG*>(&shm->header.state_flags),
          static_cast<LONG>(skydiag::kState_Loading));
      }
    } else {
      PushEvent(skydiag::EventType::kMenuClose, p, sizeof(p));

      // Best-effort: the counters may still include the closing menu; the
      // heartbeat refresh settles the flag within one interval.
      if (ui) {
        StoreInMenuFlag(*shm, *ui, false);
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

void RefreshInMenuFlag() noexcept
{
  auto* shm = GetShared();
  auto* ui = RE::UI::GetSingleton();
  if (shm && ui) {
    StoreInMenuFlag(*shm, *ui, false);
  }
}

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
