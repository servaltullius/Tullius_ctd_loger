#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Analyzer.h"
#include "MinidumpUtil.h"

namespace skydiag::dump_tool {

// ADR-0010: the objects that the crash registers and the top of the crash
// stack point at, read back from the memory the helper added to the dump
// (shared/SkyrimDiagRtti.h). Dumps written before that, and memory the helper
// could not read, give fewer or no objects. Sorted by relevance, at most
// kMaxDumpObjects.
std::vector<DumpObject> ReadDumpObjects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ThreadRecord>& threads,
  const std::vector<minidump::ModuleInfo>& modules,
  const CONTEXT& context);

inline constexpr std::size_t kMaxDumpObjects = 24;

// RBX: Character "Lydia" [0xFEAD081B] Skyrim.esm -> MyMod.esp (base TESNPC
// [0x000A2C94] Skyrim.esm), or for other objects
// "RCX: BSFadeNode (SkyrimSE.exe)". Language-neutral.
std::wstring DescribeDumpObject(const DumpObject& object);

// The first form among `objects` (most relevant first); null when none is.
const DumpObject* FirstFormObject(const std::vector<DumpObject>& objects);

// Character "Lydia" (FormID 0xFEAD081B, Skyrim.esm -> MyMod.esp): a form
// without its location. A form no plugin defines (made at runtime) names its
// base object instead: (FormID 0xFF0036F8, base TESNPC 0x000A2C94 Skyrim.esm).
// Language-neutral.
std::wstring DescribeDumpObjectTarget(const DumpObject& object);

// ADR-0010 addendum (2026-10-22): the thread a freeze verdict is about (the
// engine_spin_nan thread, the lock owner of a lock-owner wait, else the game
// thread the main thread waited for), the most relevant form it held, and the
// clue "멈춘 계산이 다루던 대상: ..." naming that form; 0 / null / empty when
// there is none. A clue only: it names no mod as the cause.
std::uint32_t FreezeVerdictThreadId(const AnalysisResult& r);
const DumpObject* FreezeVerdictForm(const AnalysisResult& r);
std::wstring FreezeTargetObjectClue(const AnalysisResult& r, bool en);

}  // namespace skydiag::dump_tool
