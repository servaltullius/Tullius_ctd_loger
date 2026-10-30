#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "CrashObjectMemory.h"
#include "SkyrimDiagHelper/DumpWriter.h"

namespace skydiag::helper::internal {

// ADR-0023: crash, freeze and manual dumps write only the game executable's
// code whole. In place of the other modules' code they get what the analyzer
// reads: the code around each thread's RIP and around each stack value that
// points into a module's code, and the headers, exports and unwind data of
// the game and of the modules those point into.
inline constexpr std::size_t kStackCodeMemoryBudget = std::size_t{ 8 } * 1024u * 1024u;
// Headers, exports and unwind data of the game and the stack modules, apart
// from the Windows modules' kModuleUnwindMemoryBudget.
inline constexpr std::size_t kStackModuleMemoryBudget = std::size_t{ 16 } * 1024u * 1024u;
inline constexpr std::chrono::milliseconds kStackCodeTimeBudget{ 200 };
// A function longer than this keeps only the window around the value.
inline constexpr std::uint32_t kMaxStackCodeFunctionBytes = 0x10000;
// Bytes kept on each side of a RIP; before and after a return address. The
// main-thread wait check reads 0x60 bytes before a return address, which can
// lie before the start of the function holding it.
inline constexpr std::uint32_t kRipWindowBytes = 0x100;
inline constexpr std::uint32_t kReturnWindowBefore = 0x100;
inline constexpr std::uint32_t kReturnWindowAfter = 0x40;
// Bytes of one thread's stack that are examined, from RSP up.
inline constexpr std::size_t kMaxStackScanBytes = std::size_t{ 1 } * 1024u * 1024u;

// One module's executable sections (RVA ranges) and .pdata, sorted by
// BeginAddress.
struct CodeImage
{
  std::uint64_t base = 0;
  std::uint32_t size = 0;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> executable;
  std::vector<RUNTIME_FUNCTION> functions;
};

// The code kept for one value that points into an executable section of
// `image`: the .pdata function holding it, when at most
// kMaxStackCodeFunctionBytes long, and a window around it (both inside the
// image). A return address belongs to the call before it, so it is looked up
// one byte back first; a RIP is looked up as is. False, and nothing added,
// when the value is not in an executable section of the image. Exposed for
// tests.
bool StackCodeRanges(const CodeImage& image, std::uint64_t value, bool isRip, std::vector<ExtraDumpMemory>& out);

// Fed by the dump writer's module and thread callbacks with what the dump
// holds; reads once, when the writer first asks for extra memory, while it
// holds the game's threads suspended.
class StackCodeMemoryCollector
{
public:
  // `preferredThreads` are examined first, in that order (the crash thread,
  // the main thread, the threads WCT saw). `exceptionRip`: the crash's own
  // RIP (0 for none); the crash thread's dump context is in the crash handler.
  StackCodeMemoryCollector(
    HANDLE process,
    std::uint64_t gameImageBase,
    std::vector<DWORD> preferredThreads,
    std::uint64_t exceptionRip,
    std::chrono::milliseconds timeBudget = kStackCodeTimeBudget);

  void OnModule(std::uint64_t base, std::uint32_t size, const wchar_t* path);
  // stackBase and stackEnd as the thread callback reports them (either order).
  void OnThread(DWORD tid, const CONTEXT& context, std::uint64_t stackBase, std::uint64_t stackEnd);
  // Reads the code and module data (the first call only) and returns their
  // ranges, merged.
  const std::vector<ExtraDumpMemory>& Collect();
  const DumpCodeMemoryStats& Stats() const { return stats_; }

private:
  struct Module
  {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::wstring path;
  };
  struct Thread
  {
    DWORD tid = 0;
    std::uint64_t rip = 0;
    std::uint64_t rsp = 0;
    std::uint64_t stackLow = 0;
    std::uint64_t stackHigh = 0;
  };

  HANDLE process_;
  std::uint64_t gameImageBase_;
  std::vector<DWORD> preferredThreads_;
  std::uint64_t exceptionRip_;
  std::chrono::milliseconds timeBudget_;
  std::vector<Module> modules_;
  std::vector<Thread> threads_;
  bool collected_ = false;
  std::vector<ExtraDumpMemory> ranges_;
  DumpCodeMemoryStats stats_{};
};

}  // namespace skydiag::helper::internal
