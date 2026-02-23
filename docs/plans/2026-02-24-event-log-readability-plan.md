# Event Log Readability Improvement Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make event log data human-readable so users can actually understand PerfHitch, MenuOpen/Close events, and see pre-freeze context summaries.

**Architecture:** Add a `detail` wstring field to EventRow, populated during analysis by `FormatEventDetail()`. A compile-time hash→name lookup table resolves FNV-1a menu hashes. OutputWriter and WinUI display `detail` when present. A new Evidence item summarizes the last few seconds before freeze/crash.

**Tech Stack:** C++20 (constexpr FNV-1a), nlohmann/json, WinUI 3 (C#/XAML)

---

### Task 1: EventRow.detail field + FormatEventDetail declaration

**Files:**
- Modify: `dump_tool/src/Analyzer.h:36-47` (add `detail` field to EventRow)
- Modify: `dump_tool/src/AnalyzerInternals.h` (add `FormatEventDetail` declaration)
- Test: `tests/event_detail_guard_tests.cpp` (new guard test)
- Modify: `tests/CMakeLists.txt` (register new test)

**Step 1: Write the failing test**

Create `tests/event_detail_guard_tests.cpp`:

```cpp
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  const std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream in(p, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestEventRowHasDetailField()
{
  const auto src = ReadFile("dump_tool/src/Analyzer.h");
  // EventRow must have a detail wstring field
  assert(src.find("std::wstring detail") != std::string::npos);
}

static void TestFormatEventDetailDeclared()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.h");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestMenuHashLookupExists()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  // Known menu hash table must exist
  assert(src.find("kKnownMenuHashes") != std::string::npos);
}

static void TestPerfHitchFormatInDetail()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  // FormatEventDetail must format PerfHitch with human-readable hitch= label
  assert(src.find("hitch=") != std::string::npos);
}

static void TestStateFlagsDecoding()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  // State flags must be decoded to human-readable names
  assert(src.find("Loading") != std::string::npos);
  assert(src.find("InMenu") != std::string::npos);
}

static void TestOutputWriterEmitsDetail()
{
  const auto src = ReadFile("dump_tool/src/OutputWriter.cpp");
  // Both text report and JSONL must include detail field
  assert(src.find("\"detail\"") != std::string::npos);
  assert(src.find("ev.detail") != std::string::npos);
}

static void TestAnalyzerPopulatesDetail()
{
  const auto src = ReadFile("dump_tool/src/Analyzer.cpp");
  assert(src.find("FormatEventDetail") != std::string::npos);
}

static void TestPreFreezeContextEvidence()
{
  const auto src = ReadFile("dump_tool/src/EvidenceBuilderInternalsEvidence.cpp");
  // Pre-freeze context summary evidence must exist
  assert(src.find("PreFreezeContext") != std::string::npos
      || src.find("pre-freeze") != std::string::npos
      || (src.find("context before") != std::string::npos || src.find("직전 상황") != std::string::npos));
}

int main()
{
  TestEventRowHasDetailField();
  TestFormatEventDetailDeclared();
  TestMenuHashLookupExists();
  TestPerfHitchFormatInDetail();
  TestStateFlagsDecoding();
  TestOutputWriterEmitsDetail();
  TestAnalyzerPopulatesDetail();
  TestPreFreezeContextEvidence();
  return 0;
}
```

**Step 2: Register test in CMakeLists.txt**

Add to `tests/CMakeLists.txt` (before the `if(NOT WIN32)` block at the end):

```cmake
add_executable(skydiag_event_detail_guard_tests
  event_detail_guard_tests.cpp
)

set_target_properties(skydiag_event_detail_guard_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

add_test(NAME skydiag_event_detail_guard_tests COMMAND skydiag_event_detail_guard_tests)
set_tests_properties(skydiag_event_detail_guard_tests PROPERTIES
  ENVIRONMENT "SKYDIAG_PROJECT_ROOT=${CMAKE_SOURCE_DIR}"
)
```

**Step 3: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R event_detail --output-on-failure`
Expected: FAIL (detail field doesn't exist yet)

**Step 4: Add detail field to EventRow**

In `dump_tool/src/Analyzer.h:36-47`, add after `d`:

```cpp
struct EventRow
{
  std::uint32_t i = 0;
  double t_ms = 0.0;
  std::uint32_t tid = 0;
  std::uint16_t type = 0;
  std::wstring type_name;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;
  std::uint64_t d = 0;
  std::wstring detail;  // human-readable summary (e.g. "hitch=105.8s flags=Loading interval=100ms")
};
```

**Step 5: Add FormatEventDetail declaration to AnalyzerInternals.h**

After `std::wstring EventTypeName(std::uint16_t t);` (line 18), add:

```cpp
std::wstring FormatEventDetail(std::uint16_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d);
```

**Step 6: Run test — only first 2 tests should pass now**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R event_detail --output-on-failure`
Expected: FAIL (remaining tests still fail — kKnownMenuHashes, hitch= format, OutputWriter detail, etc.)

**Step 7: Commit**

```bash
git add dump_tool/src/Analyzer.h dump_tool/src/AnalyzerInternals.h tests/event_detail_guard_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add EventRow.detail field + FormatEventDetail declaration + guard tests"
```

---

### Task 2: FormatEventDetail implementation — PerfHitch + MenuOpen/Close + state flags

**Files:**
- Modify: `dump_tool/src/AnalyzerInternals.cpp:1-32` (add FormatEventDetail + kKnownMenuHashes table)

**Step 1: Implement FormatEventDetail in AnalyzerInternals.cpp**

After the existing `EventTypeName()` function (line 32), add:

```cpp
namespace {

struct MenuHashEntry {
  std::uint64_t hash;
  const wchar_t* name;
};

// Pre-computed FNV-1a 64-bit hashes of known Skyrim menu names.
// Hash function: skydiag::hash::Fnv1a64(name)
// Source: RE::*Menu::MENU_NAME constants from CommonLibSSE
constexpr MenuHashEntry kKnownMenuHashes[] = {
  {14695981039346656037ull, L"(empty)"},  // Fnv1a64("")
  // -- Core UI menus --
  {0xAF63BD4C8601B7DFull, L"Console"},
  {0xAF63DE4C8601E841ull, L"Loading Menu"},
  {0xAF63DC4C8601E4D7ull, L"Main Menu"},
  {0xAF63DA4C8601E16Dull, L"Mist Menu"},
  {0xAF63AC4C860199E5ull, L"Fader Menu"},
  {0xAF63B44C8601A833ull, L"Cursor Menu"},
  // -- Gameplay menus --
  {0xAF63D84C8601DE03ull, L"HUD Menu"},
  {0xAF63D64C8601DA99ull, L"Dialogue Menu"},
  {0xAF63D44C8601D72Full, L"Inventory Menu"},
  {0xAF63D24C8601D3C5ull, L"Magic Menu"},
  {0xAF63D04C8601D05Bull, L"Map Menu"},
  {0xAF63CE4C8601CCF1ull, L"Sleep/Wait Menu"},
  {0xAF63CC4C8601C987ull, L"Container Menu"},
  {0xAF63CA4C8601C61Dull, L"Barter Menu"},
  {0xAF63C84C8601C2B3ull, L"Gift Menu"},
  {0xAF63C64C8601BF49ull, L"Lockpicking Menu"},
  {0xAF63C44C8601BBDFull, L"Book Menu"},
  {0xAF63C24C8601B875ull, L"Journal Menu"},
  {0xAF63C04C8601B50Bull, L"MessageBox Menu"},
  {0xAF63BE4C8601B1A1ull, L"Crafting Menu"},
  {0xAF63BC4C8601AE37ull, L"Training Menu"},
  {0xAF63BA4C8601AACDull, L"Tutorial Menu"},
  {0xAF63B84C8601A763ull, L"Tween Menu"},
  {0xAF63B64C8601A3F9ull, L"Stats Menu"},
  {0xAF63B24C8601A4C9ull, L"Loot Menu"},
  {0xAF63B04C8601A15Full, L"Favorites Menu"},
  {0xAF63AE4C86019DF5ull, L"Kinect Menu"},
  // -- Level up --
  {0xAF63AA4C8601967Bull, L"LevelUp Menu"},
  {0xAF63A84C86019311ull, L"StatsPage"},
  {0xAF63A64C86018FA7ull, L"Perk Menu"},
  // -- RaceMenu (popular mod) --
  {0x3526726966E2D862ull, L"RaceMenu"},
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
  return result.empty() ? L"0x" + std::to_wstring(flags) : result;
}

}  // namespace

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
      // a = FNV-1a 64-bit hash of menu name
      const wchar_t* name = LookupMenuName(a);
      if (name) {
        return name;
      }
      // Unknown hash — show hex
      wchar_t buf[32];
      std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"hash=0x%016llX", static_cast<unsigned long long>(a));
      return buf;
    }

    case EventType::kHeartbeat: {
      // a=state_flags
      return L"flags=" + DecodeStateFlags(a);
    }

    case EventType::kCellChange: {
      // a=formId (best-effort)
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
```

**Important:** The hash values above are **placeholders** — they must be computed at build time or verified. Instead of hardcoding, we'll use a **constexpr computation** approach. Since `dump_tool` doesn't include `plugin/include/SkyrimDiag/Hash.h`, we need to either:
- (a) Copy the Fnv1a64 function into AnalyzerInternals.cpp (standalone, 5 lines), or
- (b) Move Hash.h to `shared/` directory.

**Preferred approach (a):** Add a local constexpr Fnv1a64 in the anonymous namespace and compute hashes at compile time:

```cpp
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

// Compile-time FNV-1a hashes of known Skyrim menu names
constexpr MenuHashEntry kKnownMenuHashes[] = {
  {Fnv1a64(""), L"(empty)"},
  {Fnv1a64("Console"), L"Console"},
  {Fnv1a64("Loading Menu"), L"Loading Menu"},
  {Fnv1a64("Main Menu"), L"Main Menu"},
  {Fnv1a64("Mist Menu"), L"Mist Menu"},
  {Fnv1a64("Fader Menu"), L"Fader Menu"},
  {Fnv1a64("Cursor Menu"), L"Cursor Menu"},
  {Fnv1a64("HUD Menu"), L"HUD Menu"},
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
  {Fnv1a64("TutorialMenu"), L"TutorialMenu"},
  {Fnv1a64("TweenMenu"), L"TweenMenu"},
  {Fnv1a64("StatsMenu"), L"StatsMenu"},
  {Fnv1a64("LevelUp Menu"), L"LevelUp Menu"},
  {Fnv1a64("Loot Menu"), L"Loot Menu"},
  {Fnv1a64("FavoritesMenu"), L"FavoritesMenu"},
  {Fnv1a64("KinectMenu"), L"KinectMenu"},
  {Fnv1a64("RaceSex Menu"), L"RaceSex Menu"},
  // SkyUI
  {Fnv1a64("CustomMenu"), L"CustomMenu"},
  // MCM
  {Fnv1a64("Journal Menu"), L"Journal Menu"},
};

// ...rest as above (LookupMenuName, DecodeStateFlags, FormatEventDetail)
```

**Note on menu names:** The actual BSFixedString names come from `RE::*Menu::MENU_NAME` in CommonLibSSE. Key known names include: `"Console"`, `"Dialogue Menu"`, `"Fader Menu"`, `"HUD Menu"`, `"InventoryMenu"`, `"Journal Menu"`, `"Loading Menu"`, `"Main Menu"`, `"MapMenu"`, `"MessageBoxMenu"`, `"Sleep/Wait Menu"`, `"RaceSex Menu"`, `"ContainerMenu"`, `"BarterMenu"`, `"Crafting Menu"`, `"Book Menu"`, `"Lockpicking Menu"`, `"Training Menu"`, `"TweenMenu"`, `"StatsMenu"`, `"LevelUp Menu"`, `"Loot Menu"`, `"Cursor Menu"`, `"Mist Menu"`, `"GiftMenu"`, `"FavoritesMenu"`, `"TutorialMenu"`, `"KinectMenu"`, `"CustomMenu"`. The implementer should verify exact string casing from CommonLibSSE source.

**Step 2: Run test**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R event_detail --output-on-failure`
Expected: Tests for kKnownMenuHashes, hitch=, Loading, InMenu should pass

**Step 3: Commit**

```bash
git add dump_tool/src/AnalyzerInternals.cpp
git commit -m "feat: implement FormatEventDetail with menu hash lookup + PerfHitch format"
```

---

### Task 3: Wire FormatEventDetail into Analyzer.cpp + OutputWriter.cpp

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp:265-277` (call FormatEventDetail after populating EventRow)
- Modify: `dump_tool/src/OutputWriter.cpp:432-463` (emit detail in text report + JSONL)

**Step 1: Populate detail in Analyzer.cpp**

In `dump_tool/src/Analyzer.cpp`, after line 273 (`row.d = tmp.payload.d;`), add:

```cpp
        row.detail = internal::FormatEventDetail(row.type, row.a, row.b, row.c, row.d);
```

**Step 2: Update text report in OutputWriter.cpp**

Change line 434 from:
```cpp
    rpt << "[" << ev.i << "] t_ms=" << ev.t_ms << " tid=" << ev.tid << " " << WideToUtf8(ev.type_name)
        << " a=" << ev.a << " b=" << ev.b << " c=" << ev.c << " d=" << ev.d << "\n";
```
To:
```cpp
    rpt << "[" << ev.i << "] t_ms=" << ev.t_ms << " tid=" << ev.tid << " " << WideToUtf8(ev.type_name);
    if (!ev.detail.empty()) {
      rpt << " | " << WideToUtf8(ev.detail);
    }
    rpt << " a=" << ev.a << " b=" << ev.b << " c=" << ev.c << " d=" << ev.d << "\n";
```

**Step 3: Update JSONL output in OutputWriter.cpp**

Change the JSONL section (around line 447-457) — after `j["d"] = ev.d;`, add:
```cpp
      if (!ev.detail.empty()) {
        j["detail"] = WideToUtf8(ev.detail);
      }
```

**Step 4: Run test**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R event_detail --output-on-failure`
Expected: TestOutputWriterEmitsDetail and TestAnalyzerPopulatesDetail should now pass

**Step 5: Commit**

```bash
git add dump_tool/src/Analyzer.cpp dump_tool/src/OutputWriter.cpp
git commit -m "feat: wire FormatEventDetail into analysis pipeline + text/JSONL output"
```

---

### Task 4: Pre-freeze event context summary in Evidence

**Files:**
- Modify: `dump_tool/src/EvidenceBuilderInternalsPriv.h` (add helper declaration)
- Modify: `dump_tool/src/EvidenceBuilderInternalsUtil.cpp` (add BuildPreFreezeContextLine)
- Modify: `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp` (add evidence item after hitch block)

**Step 1: Add helper declaration**

In `dump_tool/src/EvidenceBuilderInternalsPriv.h`, after the `InferPerfSuspectsFromResourceCorrelation` declaration (line ~84), add:

```cpp
std::wstring BuildPreFreezeContextLine(const std::vector<EventRow>& events, bool en);
```

**Step 2: Implement BuildPreFreezeContextLine**

In `dump_tool/src/EvidenceBuilderInternalsUtil.cpp`, after the `FindLastEventTimeMsByType` function, add:

```cpp
std::wstring BuildPreFreezeContextLine(const std::vector<EventRow>& events, bool en)
{
  if (events.empty()) return {};

  // Find the last PerfHitch with a >= 2000ms (big hitch)
  const EventRow* lastBigHitch = nullptr;
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type == static_cast<std::uint16_t>(skydiag::EventType::kPerfHitch) && it->a >= 2000) {
      lastBigHitch = &(*it);
      break;
    }
  }

  if (!lastBigHitch) return {};

  // Collect interesting events in the 10 seconds before the big hitch
  const double hitchTime = lastBigHitch->t_ms;
  const double windowMs = 10000.0;

  std::vector<std::wstring> context;
  for (const auto& e : events) {
    if (e.t_ms > hitchTime) break;
    if (e.t_ms < hitchTime - windowMs) continue;
    if (&e == lastBigHitch) break;

    switch (static_cast<skydiag::EventType>(e.type)) {
      case skydiag::EventType::kMenuOpen:
      case skydiag::EventType::kMenuClose:
      case skydiag::EventType::kLoadStart:
      case skydiag::EventType::kLoadEnd:
      case skydiag::EventType::kCellChange:
      case skydiag::EventType::kPerfHitch: {
        std::wstring line = e.type_name;
        if (!e.detail.empty()) {
          line += L"(" + e.detail + L")";
        }
        context.push_back(std::move(line));
        if (context.size() >= 5) break;
        continue;
      }
      default:
        continue;
    }
    if (context.size() >= 5) break;
  }

  if (context.empty()) return {};

  std::wstring result;
  for (std::size_t i = 0; i < context.size(); i++) {
    if (i > 0) result += L" → ";
    result += context[i];
  }
  result += en
    ? (L" → PerfHitch(" + lastBigHitch->detail + L")")
    : (L" → PerfHitch(" + lastBigHitch->detail + L")");

  return result;
}
```

**Step 3: Add evidence item**

In `dump_tool/src/EvidenceBuilderInternalsEvidence.cpp`, after the hitch summary evidence block (after the hitch resource correlation block, around line 350), add:

```cpp
  // Pre-freeze context summary: what happened in the seconds before the biggest hitch
  if (isHangLike || (hitch.count > 0 && hitch.maxMs >= 2000)) {
    const auto preFreeze = BuildPreFreezeContextLine(r.events, en);
    if (!preFreeze.empty()) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en
        ? L"Context before freeze / big hitch"
        : L"프리징/큰 히치 직전 상황";
      e.details = preFreeze;
      r.evidence.push_back(std::move(e));
    }
  }
```

**Step 4: Run all tests**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL tests pass (including the new guard test checking for pre-freeze context)

**Step 5: Commit**

```bash
git add dump_tool/src/EvidenceBuilderInternalsPriv.h dump_tool/src/EvidenceBuilderInternalsUtil.cpp dump_tool/src/EvidenceBuilderInternalsEvidence.cpp
git commit -m "feat: add pre-freeze event context summary in Evidence"
```

---

### Task 5: WinUI — display detail field in event lines

**Files:**
- Modify: `dump_tool_winui/MainWindow.xaml.cs:643-701` (parse `detail` from JSONL, display it)

**Step 1: Update LoadAdvancedArtifactsCore**

In `dump_tool_winui/MainWindow.xaml.cs`, the JSONL lines are currently displayed raw. The WinUI reads the raw JSONL and stores lines as strings. We need to parse each JSONL line and format it with the detail field.

Find the section where `tail.Enqueue(line)` happens (around line 668). Change it to parse the JSON and format:

```csharp
// Replace the raw line enqueue with formatted display
try
{
    using var jDoc = JsonDocument.Parse(line);
    var root = jDoc.RootElement;
    var idx = root.GetProperty("i").GetInt32();
    var tMs = root.GetProperty("t_ms").GetDouble();
    var tid = root.GetProperty("tid").GetUInt32();
    var typeName = root.GetProperty("type_name").GetString() ?? "?";
    var detail = root.TryGetProperty("detail", out var detProp) ? detProp.GetString() : null;

    string formatted;
    if (!string.IsNullOrEmpty(detail))
    {
        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} | {detail}";
    }
    else
    {
        var a = root.GetProperty("a").GetUInt64();
        var b = root.GetProperty("b").GetUInt64();
        formatted = $"[{idx}] t={tMs:F0}ms tid={tid} {typeName} a={a} b={b}";
    }
    tail.Enqueue(formatted);
}
catch
{
    tail.Enqueue(line);  // fallback: raw line
}
```

**Step 2: Commit**

This change is WinUI-only — cannot be tested on Linux. Verify visually after Windows build.

```bash
git add dump_tool_winui/MainWindow.xaml.cs
git commit -m "feat: WinUI event display uses detail field for human-readable output"
```

---

### Task 6: Verify menu name hashes against CommonLibSSE source

**Files:**
- Read-only verification task (web research)

**Step 1: Verify known menu names**

The implementer should verify menu name strings from CommonLibSSE-NG headers. Key references:
- `include/RE/I/IMenu.h` and related `*Menu.h` files
- Each has a `static constexpr auto MENU_NAME` defining the BSFixedString

Known verified names (from CommonLibSSE-NG source):
- `"Console"` (Console)
- `"Dialogue Menu"` (DialogueMenu)
- `"Fader Menu"` (FaderMenu)
- `"HUD Menu"` (HUDMenu)
- `"InventoryMenu"` (InventoryMenu)
- `"Journal Menu"` (JournalMenu)
- `"Loading Menu"` (LoadingMenu)
- `"Main Menu"` (MainMenu)
- `"MapMenu"` (MapMenu)
- `"MessageBoxMenu"` (MessageBoxMenu)
- `"Sleep/Wait Menu"` (SleepWaitMenu)
- `"RaceSex Menu"` (RaceSexMenu)
- `"ContainerMenu"` (ContainerMenu)
- `"BarterMenu"` (BarterMenu)
- `"Crafting Menu"` (CraftingMenu)
- `"Book Menu"` (BookMenu)
- `"Lockpicking Menu"` (LockpickingMenu)
- `"Training Menu"` (TrainingMenu)
- `"TweenMenu"` (TweenMenu)
- `"StatsMenu"` (StatsMenu)
- `"LevelUp Menu"` (LevelUpMenu)
- `"Loot Menu"` (LootMenu)
- `"Cursor Menu"` (CursorMenu)
- `"Mist Menu"` (MistMenu)
- `"GiftMenu"` (GiftMenu)
- `"FavoritesMenu"` (FavoritesMenu)
- `"TutorialMenu"` (TutorialMenu)
- `"KinectMenu"` (KinectMenu)
- `"CustomMenu"` (SkyUI custom overlay)

Since these are used in constexpr Fnv1a64() calls, any typo will produce wrong hashes at compile time. The strings must match **exactly** what the game engine sends through BSFixedString.

**Step 2: If any names are wrong, fix in AnalyzerInternals.cpp and re-run tests**

---

### Summary of all tasks

| Task | Description | Tests | Commit |
|------|-------------|-------|--------|
| 1 | EventRow.detail + FormatEventDetail declaration + guard tests | event_detail_guard_tests (8 asserts) | `feat: add EventRow.detail field...` |
| 2 | FormatEventDetail implementation (PerfHitch/Menu/Heartbeat/CellChange) | guard tests pass (hitch=, Loading, kKnownMenuHashes) | `feat: implement FormatEventDetail...` |
| 3 | Wire into Analyzer.cpp + OutputWriter.cpp | guard tests pass (ev.detail, "detail") | `feat: wire FormatEventDetail...` |
| 4 | Pre-freeze context summary Evidence | guard test pass (직전 상황) | `feat: add pre-freeze event context...` |
| 5 | WinUI detail display | Visual verification (Windows only) | `feat: WinUI event display...` |
| 6 | Menu name hash verification | Research only | (amend Task 2 if needed) |
