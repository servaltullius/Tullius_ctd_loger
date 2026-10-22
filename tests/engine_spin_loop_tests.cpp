// engine_spin_nan's code and register reads (EngineSpinLoop.h), on synthetic
// code windows and thread contexts shaped like the 2026-10-22 freeze: a game
// thread at SkyrimSE.exe+0xd17465 in "addss xmm0,xmm6; comiss xmm0,xmm7;
// jb -9" (while (x < 0) x += 2*pi) with xmm0 = 0x7FE500DD (NaN), xmm6 = 2*pi.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "EngineSpinLoop.h"

using skydiag::dump_tool::FindFloatCompareLoop;
using skydiag::dump_tool::FindNaNCompareRegister;
using skydiag::dump_tool::FloatCompareMnemonic;
using skydiag::dump_tool::IsNaNBits;
using skydiag::dump_tool::kContextMxCsrOffset;
using skydiag::dump_tool::kContextXmmRegistersOffset;
using skydiag::dump_tool::ReadContextMxCsr;
using skydiag::dump_tool::ReadContextXmmLowLane;

namespace {

constexpr std::size_t kContextSize = 0x4D0;  // sizeof(CONTEXT) on x64
constexpr std::uint32_t kFieldNaN = 0x7FE500DDu;
constexpr std::uint32_t kTwoPi = 0x40C90FDBu;

// Where Window puts the loop's first instruction.
constexpr std::size_t kLoopStart = 16;

// kLoopStart bytes of filler (int3), the loop, then filler again.
std::vector<std::uint8_t> Window(const std::vector<std::uint8_t>& loop)
{
  std::vector<std::uint8_t> code(kLoopStart, 0xCC);
  code.insert(code.end(), loop.begin(), loop.end());
  code.insert(code.end(), 16, 0xCC);
  return code;
}

// addss xmm0,xmm6 (F3 0F 58 C6); comiss xmm0,xmm7 (0F 2F C7); jb -9 (72 F7).
const std::vector<std::uint8_t> kFieldLoop = { 0xF3, 0x0F, 0x58, 0xC6, 0x0F, 0x2F, 0xC7, 0x72, 0xF7 };

std::vector<std::uint8_t> Context()
{
  return std::vector<std::uint8_t>(kContextSize, 0);
}

void SetXmm(std::vector<std::uint8_t>& context, unsigned reg, std::uint32_t low)
{
  std::memcpy(context.data() + kContextXmmRegistersOffset + reg * 16u, &low, sizeof(low));
}

void TestFieldLoopIsFound()
{
  const std::size_t start = kLoopStart;
  const auto code = Window(kFieldLoop);
  // The thread can stop on any of the three instructions.
  for (const std::size_t rip : { start, start + 4u, start + 7u }) {
    const auto loop = FindFloatCompareLoop(code, rip);
    assert(loop.found);
    assert(!loop.is_double && !loop.unordered_compare);
    assert(loop.reg == 0u && loop.has_rm_reg && loop.rm_reg == 7u);
    assert(loop.compare_offset == start + 4u);
    assert(FloatCompareMnemonic(loop) == "comiss");
  }
  // Past the jump or before the loop, the thread is not in it.
  assert(!FindFloatCompareLoop(code, start + 9u).found);
  assert(!FindFloatCompareLoop(code, start - 1u).found);
  // The instruction pointer must lie in the window.
  assert(!FindFloatCompareLoop(code, code.size()).found);
  assert(!FindFloatCompareLoop({}, 0).found);
}

void TestOnlyLoopsNaNCannotLeave()
{
  const std::size_t start = kLoopStart;
  // ja (77): an unordered compare does not take it, so NaN leaves the loop.
  auto ja = kFieldLoop;
  ja[7] = 0x77;
  assert(!FindFloatCompareLoop(Window(ja), start).found);
  // jae (73) and jne (75) likewise.
  auto jae = kFieldLoop;
  jae[7] = 0x73;
  assert(!FindFloatCompareLoop(Window(jae), start).found);
  // jbe, je and jp are taken on unordered.
  for (const std::uint8_t op : { std::uint8_t{ 0x76 }, std::uint8_t{ 0x74 }, std::uint8_t{ 0x7A } }) {
    auto taken = kFieldLoop;
    taken[7] = op;
    assert(FindFloatCompareLoop(Window(taken), start).found);
  }
  // A forward jump is no loop.
  auto forward = kFieldLoop;
  forward[8] = 0x05;
  assert(!FindFloatCompareLoop(Window(forward), start).found);
  // A compare not followed by the jump.
  const std::vector<std::uint8_t> apart = { 0xF3, 0x0F, 0x58, 0xC6, 0x0F, 0x2F, 0xC7, 0x90, 0x72, 0xF6 };
  assert(!FindFloatCompareLoop(Window(apart), start).found);
  // Not a compare at all: addss; subss; jb back.
  const std::vector<std::uint8_t> noCompare = { 0xF3, 0x0F, 0x58, 0xC6, 0xF3, 0x0F, 0x5C, 0xC7, 0x72, 0xF6 };
  assert(!FindFloatCompareLoop(Window(noCompare), start).found);
}

void TestOtherEncodings()
{
  const std::size_t start = kLoopStart;
  // ucomisd xmm8, xmm15 (66 45 0F 2E C7) with jp rel32 back to the compare.
  const std::vector<std::uint8_t> ucomisd = { 0x66, 0x45, 0x0F, 0x2E, 0xC7, 0x0F, 0x8A, 0xF5, 0xFF, 0xFF, 0xFF };
  auto loop = FindFloatCompareLoop(Window(ucomisd), start + 5u);
  assert(loop.found && loop.is_double && loop.unordered_compare);
  assert(loop.reg == 8u && loop.rm_reg == 15u);
  assert(FloatCompareMnemonic(loop) == "ucomisd");

  // comiss xmm1, [rip+disp32] (0F 2F 0D xx xx xx xx): only the register operand.
  const std::vector<std::uint8_t> memory = { 0xF3, 0x0F, 0x58, 0xCE, 0x0F, 0x2F, 0x0D, 0x10, 0x20, 0x30, 0x00, 0x72, 0xF3 };
  loop = FindFloatCompareLoop(Window(memory), start);
  assert(loop.found && loop.reg == 1u && !loop.has_rm_reg);

  // A jump back further than a short loop is not taken for one.
  std::vector<std::uint8_t> longLoop(80, 0x90);
  longLoop.insert(longLoop.end(), { 0x0F, 0x2F, 0xC7, 0x0F, 0x82 });
  const std::int32_t rel = -static_cast<std::int32_t>(longLoop.size() + 4u);
  std::uint8_t relBytes[sizeof(rel)]{};
  std::memcpy(relBytes, &rel, sizeof(rel));
  longLoop.insert(longLoop.end(), std::begin(relBytes), std::end(relBytes));
  assert(!FindFloatCompareLoop(Window(longLoop), start + 82u).found);

  // A jump cut off by the end of the window.
  const std::vector<std::uint8_t> cut = { 0x0F, 0x2F, 0xC7, 0x0F, 0x82, 0xF7 };
  assert(!FindFloatCompareLoop(cut, 0).found);
}

void TestXmmReadIsBounded()
{
  auto context = Context();
  SetXmm(context, 0, kFieldNaN);
  SetXmm(context, 6, kTwoPi);
  SetXmm(context, 15, 0x3F800000u);
  assert(ReadContextXmmLowLane(context.data(), context.size(), 0, false) == kFieldNaN);
  assert(ReadContextXmmLowLane(context.data(), context.size(), 6, false) == kTwoPi);
  assert(ReadContextXmmLowLane(context.data(), context.size(), 15, false) == 0x3F800000u);
  // Register numbers past xmm15, a context that ends before the register, a
  // null context.
  assert(!ReadContextXmmLowLane(context.data(), context.size(), 16, false));
  assert(!ReadContextXmmLowLane(context.data(), kContextXmmRegistersOffset, 0, false));
  assert(!ReadContextXmmLowLane(context.data(), kContextXmmRegistersOffset + 3u, 0, false));
  assert(ReadContextXmmLowLane(context.data(), kContextXmmRegistersOffset + 4u, 0, false) == kFieldNaN);
  assert(!ReadContextXmmLowLane(context.data(), kContextXmmRegistersOffset + 4u, 0, true));
  assert(!ReadContextXmmLowLane(context.data(), kContextXmmRegistersOffset + 15u * 16u, 15, false));
  assert(!ReadContextXmmLowLane(nullptr, context.size(), 0, false));

  const std::uint32_t mxcsr = 0x1FBFu;
  std::memcpy(context.data() + kContextMxCsrOffset, &mxcsr, sizeof(mxcsr));
  assert(ReadContextMxCsr(context.data(), context.size()) == 0x1FBFu);
  assert(!ReadContextMxCsr(context.data(), kContextMxCsrOffset + 3u));
}

void TestNaNBits()
{
  assert(IsNaNBits(kFieldNaN, false));
  assert(IsNaNBits(0x7FC00000u, false) && IsNaNBits(0xFFC00000u, false));
  assert(!IsNaNBits(0x7F800000u, false));  // +inf
  assert(!IsNaNBits(kTwoPi, false) && !IsNaNBits(0, false));
  assert(!IsNaNBits(0x1'7FC00000ull, false));  // more than a float
  assert(IsNaNBits(0x7FF8000000000000ull, true));
  assert(!IsNaNBits(0x7FF0000000000000ull, true));  // +inf
  assert(!IsNaNBits(0x401921FB54442D18ull, true));  // 2*pi
}

void TestNaNCompareRegister()
{
  const std::size_t start = kLoopStart;
  const auto loop = FindFloatCompareLoop(Window(kFieldLoop), start);
  auto context = Context();
  SetXmm(context, 6, kTwoPi);
  assert(!FindNaNCompareRegister(loop, context.data(), context.size()));

  SetXmm(context, 0, kFieldNaN);
  std::uint64_t bits = 0;
  assert(FindNaNCompareRegister(loop, context.data(), context.size(), &bits) == 0u);
  assert(bits == kFieldNaN);

  // The second operand holding NaN counts too.
  SetXmm(context, 0, 0xBF800000u);
  SetXmm(context, 7, 0x7FC00000u);
  assert(FindNaNCompareRegister(loop, context.data(), context.size()) == 7u);

  // A context cut short before the registers names none.
  assert(!FindNaNCompareRegister(loop, context.data(), kContextXmmRegistersOffset));
  assert(!FindNaNCompareRegister({}, context.data(), context.size()));
}

}  // namespace

int main()
{
  TestFieldLoopIsFound();
  TestOnlyLoopsNaNCannotLeave();
  TestOtherEncodings();
  TestXmmReadIsBounded();
  TestNaNBits();
  TestNaNCompareRegister();
  return 0;
}
