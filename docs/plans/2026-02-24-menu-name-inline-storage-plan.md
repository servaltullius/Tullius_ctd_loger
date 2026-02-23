# Menu Name Inline Storage Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Store the actual menu name string in the event payload so all mod menus display their real name instead of an opaque hash.

**Architecture:** Plugin packs the UTF-8 menu name into the unused 24 bytes (b+c+d) of EventPayload. Analyzer extracts the string if present, falling back to the existing hash lookup table for old dumps.

**Tech Stack:** C++20, memcpy into trivially_copyable struct, no new dependencies

---

### Task 1: Guard test + Plugin stores menu name string in payload

**Files:**
- Modify: `tests/event_detail_guard_tests.cpp` (add guard assertion)
- Modify: `plugin/src/EventSinks.cpp:36-37` (pack menu name into b+c+d)
- Modify: `dump_tool/src/AnalyzerInternals.cpp:141-151` (extract string from b+c+d)

**Step 1: Add guard test assertion**

In `tests/event_detail_guard_tests.cpp`, add a new test function before `main()`:

```cpp
static void TestPluginStoresMenuNameInPayload()
{
  const auto src = ReadFile("plugin/src/EventSinks.cpp");
  // Plugin must copy menu name string into payload bytes
  assert(src.find("memcpy") != std::string::npos);
}

static void TestAnalyzerExtractsMenuNameFromPayload()
{
  const auto src = ReadFile("dump_tool/src/AnalyzerInternals.cpp");
  // Analyzer must extract embedded menu name from b+c+d bytes
  assert(src.find("menuBuf") != std::string::npos || src.find("menu_buf") != std::string::npos);
}
```

And add the calls in `main()`:

```cpp
  TestPluginStoresMenuNameInPayload();
  TestAnalyzerExtractsMenuNameFromPayload();
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test -R event_detail --output-on-failure`
Expected: FAIL (memcpy not in EventSinks.cpp, menuBuf not in AnalyzerInternals.cpp)

**Step 3: Modify Plugin EventSinks.cpp**

In `plugin/src/EventSinks.cpp`, add `#include <cstring>` to the top includes. Then change lines 36-37 from:

```cpp
    skydiag::EventPayload p{};
    p.a = menuHash;
```

To:

```cpp
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
```

Note: `p` is zero-initialized (`{}`), so `dst` is already all zeros. The `if (len > 0)` avoids a memcpy of zero bytes. If `len < 24`, the null terminator is already there from zero-init. If `len >= 24`, we truncate and write explicit null at position 23.

**Step 4: Modify FormatEventDetail in AnalyzerInternals.cpp**

Change the `kMenuOpen`/`kMenuClose` case (lines 141-151) from:

```cpp
    case EventType::kMenuOpen:
    case EventType::kMenuClose: {
      // a = FNV-1a 64-bit hash of menu name
      const wchar_t* name = LookupMenuName(a);
      if (name) {
        return name;
      }
      wchar_t buf[32];
      std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"hash=0x%016llX", static_cast<unsigned long long>(a));
      return buf;
    }
```

To:

```cpp
    case EventType::kMenuOpen:
    case EventType::kMenuClose: {
      // Try embedded menu name string first (b+c+d = 24 bytes UTF-8)
      char menuBuf[24]{};
      std::memcpy(menuBuf, &b, 8);
      std::memcpy(menuBuf + 8, &c, 8);
      std::memcpy(menuBuf + 16, &d, 8);
      menuBuf[23] = '\0';  // safety null
      if (menuBuf[0] != '\0') {
        // Convert UTF-8 to wstring
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
```

Note: The char-by-char widen is sufficient because Skyrim menu names are ASCII-only (no multibyte UTF-8 in known menu names). The `#include <cstring>` is already present in this file via other headers; if not, add it.

**Step 5: Build and test**

Run: `cmake --build build-linux-test -j && ctest --test-dir build-linux-test --output-on-failure`
Expected: ALL tests pass (41/41 + 2 new = still reported as 41 since it's the same test binary)

**Step 6: Commit**

```bash
git add plugin/src/EventSinks.cpp dump_tool/src/AnalyzerInternals.cpp tests/event_detail_guard_tests.cpp
git commit -m "feat: store menu name string in event payload for mod menu identification"
```

---

### Summary

| Task | Description | Files | Lines changed |
|------|-------------|-------|---------------|
| 1 | Guard tests + plugin packs name + analyzer extracts name | EventSinks.cpp, AnalyzerInternals.cpp, guard tests | ~25 |

Single task — the change is small enough that splitting further would be over-engineering.
