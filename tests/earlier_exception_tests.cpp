// ADR-0039: the exception the crashing thread was handling when the reported
// one happened. The fixture is the 2026-11-28 crash: an access violation
// (read of 0x1DFD5DF3000) at Precision.dll+0xe5547 was being dispatched when
// the vectored handler of a frame generation DLL (nvngx_dlssg.dll) hit heap
// corruption (0xC0000374) in ntdll. The saved CONTEXT lay at the dispatcher
// frame's stack pointer 0x38559FE400 and the EXCEPTION_RECORD 0x4F0 after it
// (0x38559FE8F0), as read from that dump.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "EarlierException.h"

namespace {

namespace ee = skydiag::dump_tool::earlier_exception;
using skydiag::dump_tool::EarlierExceptionInfo;

constexpr std::uint64_t kNtdllBase = 0x7FFD04D50000ull;
constexpr std::uint64_t kPrecisionBase = 0x7FFBD2970000ull;
constexpr std::uint64_t kHandlerBase = 0x7FFB9D360000ull;  // nvngx_dlssg.dll
constexpr std::uint64_t kFirstFault = kPrecisionBase + 0xE5547ull;
constexpr std::uint64_t kAccessedAddress = 0x1DFD5DF3000ull;
constexpr std::uint64_t kDispatcherSp = 0x38559FE400ull;
constexpr std::uint64_t kFaultingSp = 0x38559FF170ull;
constexpr std::size_t kDispatcherIndex = 10;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(std::wstring_view text, std::wstring_view part)
{
  return text.find(part) != std::wstring_view::npos;
}

// Sparse little-endian memory: what the dump holds, and nothing else.
struct FakeMemory
{
  std::map<std::uint64_t, std::uint8_t> bytes;

  void Put32(std::uint64_t address, std::uint32_t value)
  {
    for (int i = 0; i < 4; ++i) {
      bytes[address + static_cast<std::uint64_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }
  void Put64(std::uint64_t address, std::uint64_t value)
  {
    for (int i = 0; i < 8; ++i) {
      bytes[address + static_cast<std::uint64_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }
  // Zeroes, as the dump has for a stack range it holds.
  void Fill(std::uint64_t from, std::uint64_t to)
  {
    for (std::uint64_t a = from; a < to; ++a) {
      bytes.emplace(a, std::uint8_t{ 0 });
    }
  }
  ee::ReadMemory Reader() const
  {
    return [this](std::uint64_t address, void* out, std::size_t size) {
      auto* dst = static_cast<std::uint8_t*>(out);
      for (std::size_t i = 0; i < size; ++i) {
        const auto it = bytes.find(address + i);
        if (it == bytes.end()) {
          return false;
        }
        dst[i] = it->second;
      }
      return true;
    };
  }
};

void PutContext(
  FakeMemory& mem,
  std::uint64_t at,
  std::uint32_t flags,
  std::uint64_t rsp,
  std::uint64_t rip,
  std::uint16_t segCs = ee::kUserCodeSegment)
{
  mem.Put32(at + ee::kContextFlagsOffset, flags);
  mem.bytes[at + ee::kContextSegCsOffset] = static_cast<std::uint8_t>(segCs);
  mem.bytes[at + ee::kContextSegCsOffset + 1u] = static_cast<std::uint8_t>(segCs >> 8);
  mem.Put64(at + ee::kContextRspOffset, rsp);
  mem.Put64(at + ee::kContextRipOffset, rip);
}

void PutRecord(
  FakeMemory& mem,
  std::uint64_t at,
  std::uint32_t code,
  std::uint64_t address,
  const std::vector<std::uint64_t>& params,
  std::uint32_t flags = 0u)
{
  mem.Put32(at, code);
  mem.Put32(at + 4u, flags);
  mem.Put64(at + 8u, 0u);
  mem.Put64(at + ee::kRecordAddressOffset, address);
  mem.Put32(at + ee::kRecordParameterCountOffset, static_cast<std::uint32_t>(params.size()));
  for (std::size_t i = 0; i < params.size(); ++i) {
    mem.Put64(at + ee::kRecordParametersOffset + 8u * i, params[i]);
  }
}

// The crash stack of 2026-11-28: ntdll's critical failure path (0-4), the
// handler in nvngx_dlssg.dll (5-7), ntdll's vectored handler call (8-9), the
// dispatcher (10), then Precision.dll from the first fault on (11-14).
std::vector<ee::Frame> CrashFrames()
{
  std::vector<ee::Frame> frames;
  std::uint64_t sp = 0x38559FC000ull;
  const auto add = [&](std::uint64_t pc, bool ntdll) {
    frames.push_back(ee::Frame{ pc, sp, ntdll });
    sp += 0x80u;
  };
  add(kNtdllBase + 0x112FC5ull, true);
  add(kNtdllBase + 0xF1A35ull, true);
  add(kNtdllBase + 0x11E602ull, true);
  add(kNtdllBase + 0x1094DAull, true);
  add(kNtdllBase + 0x2B527ull, true);
  add(kHandlerBase + 0x2C01FCull, false);
  add(kHandlerBase + 0x1626Full, false);
  add(kHandlerBase + 0x1565Aull, false);
  add(kNtdllBase + 0xA3416ull, true);
  add(kNtdllBase + 0xA2BB5ull, true);
  frames.push_back(ee::Frame{ kNtdllBase + 0x9F2DEull, kDispatcherSp, true });  // KiUserExceptionDispatcher+0x2e
  frames.push_back(ee::Frame{ kFirstFault, kFaultingSp, false });
  frames.push_back(ee::Frame{ kPrecisionBase + 0xF17E3ull, kFaultingSp + 0x60u, false });
  frames.push_back(ee::Frame{ kPrecisionBase + 0xF4E5Eull, kFaultingSp + 0xC0u, false });
  frames.push_back(ee::Frame{ kPrecisionBase + 0x11F5D2ull, kFaultingSp + 0x120u, false });
  return frames;
}

// The dump's bytes there: the stack from the dispatcher's frame to the
// faulting frame, with the CONTEXT and the record where Windows put them.
FakeMemory CrashMemory(std::uint64_t recordOffset = 0x4F0u)
{
  FakeMemory mem;
  mem.Fill(kDispatcherSp, kFaultingSp + 0x200u);
  PutContext(mem, kDispatcherSp, 0x0010005Fu, kFaultingSp, kFirstFault);
  PutRecord(mem, kDispatcherSp + recordOffset, 0xC0000005u, kFirstFault, { 0u, kAccessedAddress });
  return mem;
}

void TestFindsTheRealLayout()
{
  const auto mem = CrashMemory();
  const auto found = ee::FindEarliest(CrashFrames(), mem.Reader());
  Require(found.has_value(), "the 2026-11-28 record is found");
  Require(found->dispatcher_index == kDispatcherIndex, "at the dispatcher's frame");
  Require(found->context_address == kDispatcherSp, "the CONTEXT at its stack pointer");
  Require(found->record_address == 0x38559FE8F0ull, "the record 0x4F0 after it");
  Require(found->code == 0xC0000005u && found->flags == 0u, "an access violation");
  Require(found->address == kFirstFault, "at Precision.dll+0xe5547");
  Require(found->info.size() == 2u && found->info[0] == 0u && found->info[1] == kAccessedAddress, "a read of 0x1DFD5DF3000");
}

void TestRecordElsewhereInTheWindow()
{
  // Another extended-state layout may put the record further on.
  const auto mem = CrashMemory(0x5A0u);
  const auto found = ee::FindEarliest(CrashFrames(), mem.Reader());
  Require(found && found->record_address == kDispatcherSp + 0x5A0u, "a record further on is found");

  const auto far = CrashMemory(ee::kContextSize + ee::kRecordSearchBytes + 0x10u);
  Require(!ee::FindEarliest(CrashFrames(), far.Reader()), "not past the window");
}

void TestNothingWithoutAMatchingContext()
{
  {
    // A CONTEXT and a record that agree with each other but not with the
    // next frame: stale data from another dispatch, not this one.
    auto mem = CrashMemory();
    mem.Put64(kDispatcherSp + ee::kContextRipOffset, kFirstFault + 4u);
    mem.Put64(kDispatcherSp + 0x4F0u + ee::kRecordAddressOffset, kFirstFault + 4u);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "the CONTEXT's Rip must be the next frame's pc");
  }
  {
    auto mem = CrashMemory();
    mem.Put32(kDispatcherSp + ee::kContextFlagsOffset, 0x0000005Fu);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "a CONTEXT_AMD64 context");
  }
  {
    auto mem = CrashMemory();
    mem.Put32(kDispatcherSp + ee::kContextFlagsOffset, 0x0010005Eu);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "with its control registers");
  }
  {
    auto mem = CrashMemory();
    mem.Put64(kDispatcherSp + ee::kContextRspOffset, kDispatcherSp - 0x10u);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "the faulting frame lies above the dispatcher's");
  }
  {
    auto frames = CrashFrames();
    frames[kDispatcherIndex].may_be_dispatcher = false;
    Require(!ee::FindEarliest(frames, CrashMemory().Reader()), "only a frame in ntdll.dll");
  }
  {
    FakeMemory empty;
    Require(!ee::FindEarliest(CrashFrames(), empty.Reader()), "memory the dump does not hold");
  }
  {
    auto mem = CrashMemory();
    mem.bytes[kDispatcherSp + ee::kContextSegCsOffset] = 0x10;  // the kernel's code segment
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "a thread interrupted in 64-bit user mode");
  }
  {
    // The walk's last frame accepts any Rip, but only one in user space.
    auto frames = CrashFrames();
    frames.resize(kDispatcherIndex + 1u);
    for (const std::uint64_t rip : { 0x500ull, 0xFFFFF80000001000ull }) {
      auto mem = CrashMemory();
      mem.Put64(kDispatcherSp + ee::kContextRipOffset, rip);
      mem.Put64(kDispatcherSp + 0x4F0u + ee::kRecordAddressOffset, rip);
      Require(!ee::FindEarliest(frames, mem.Reader()), "a Rip in user space");
    }
  }
  {
    // A corrupt stack pointer at the top of the address space: no window
    // wrapping around, and the search ends.
    auto frames = CrashFrames();
    const std::uint64_t top = ~std::uint64_t{ 0 } - ee::kContextSize - ee::kRecordSearchBytes;
    frames[kDispatcherIndex].sp = top;
    FakeMemory mem;
    mem.Fill(top, top + ee::kContextRipOffset + 8u);
    PutContext(mem, top, 0x0010005Fu, ~std::uint64_t{ 0 }, kFirstFault);
    Require(!ee::FindEarliest(frames, mem.Reader()), "no stack pointer outside user space");
  }
}

void TestNothingWithoutAMatchingRecord()
{
  {
    auto mem = CrashMemory();
    mem.Put64(kDispatcherSp + 0x4F0u + ee::kRecordAddressOffset, kFirstFault + 4u);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "the record's address must be the Rip");
  }
  {
    auto mem = CrashMemory();
    mem.Put32(kDispatcherSp + 0x4F0u + ee::kRecordParameterCountOffset, 16u);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "at most 15 parameters");
  }
  for (const std::uint32_t code : { 0x80000003u, 0x80000004u, 0x80000001u, 0x40010006u, 0u }) {
    auto mem = CrashMemory();
    mem.Put32(kDispatcherSp + 0x4F0u, code);
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "only an error exception (no breakpoint, single step or guard page)");
  }
  {
    // The breakpoint at the record's place is what was dispatched: an older
    // error record at the same address further on is stale.
    auto mem = CrashMemory();
    mem.Put32(kDispatcherSp + 0x4F0u, 0x80000003u);
    PutRecord(mem, kDispatcherSp + 0x5A0u, 0xC0000005u, kFirstFault, { 0u, kAccessedAddress });
    Require(!ee::FindEarliest(CrashFrames(), mem.Reader()), "the first record at the Rip decides");
  }
  {
    auto mem = CrashMemory();
    PutRecord(mem, kDispatcherSp + 0x4F0u, 0xC0000005u, kFirstFault, { 0u, kAccessedAddress }, /*flags=*/1u);
    const auto found = ee::FindEarliest(CrashFrames(), mem.Reader());
    Require(found && found->flags == 1u, "the record's flags are kept (EXCEPTION_NONCONTINUABLE)");
  }
}

void TestTheDeepestDispatcherWins()
{
  // Two exceptions handled one inside the other: the deeper one came first.
  auto frames = CrashFrames();
  auto mem = CrashMemory();
  const std::uint64_t upperSp = frames[3].sp;  // an ntdll frame above the handler
  const std::uint64_t upperRip = frames[4].pc;
  mem.Fill(upperSp, upperSp + 0x900u);
  PutContext(mem, upperSp, 0x0010001Fu, frames[4].sp, upperRip);
  PutRecord(mem, upperSp + 0x4F0u, 0xC000001Du, upperRip, {});
  const auto found = ee::FindEarliest(frames, mem.Reader());
  Require(found && found->dispatcher_index == kDispatcherIndex && found->code == 0xC0000005u, "the first exception");

  // Without the deeper one the upper one is found.
  frames[kDispatcherIndex].may_be_dispatcher = false;
  const auto upper = ee::FindEarliest(frames, mem.Reader());
  Require(upper && upper->dispatcher_index == 3u && upper->code == 0xC000001Du, "the upper dispatcher");
}

void TestDispatcherAtTheEndOfTheWalk()
{
  // The walk stopped at the dispatcher: the CONTEXT and the record still say
  // where the first exception happened.
  auto frames = CrashFrames();
  frames.resize(kDispatcherIndex + 1u);
  const auto found = ee::FindEarliest(frames, CrashMemory().Reader());
  Require(found && found->address == kFirstFault, "found from the CONTEXT alone");
}

void TestTheTopFrameIsNeverTheDispatcher()
{
  // Frame 0 is the reported exception's own place.
  auto frames = CrashFrames();
  std::vector<ee::Frame> top(frames.begin() + static_cast<std::ptrdiff_t>(kDispatcherIndex), frames.end());
  Require(!ee::FindEarliest(top, CrashMemory().Reader()), "frame 0 is not looked at");
}

void TestHandlerFrame()
{
  const auto frames = CrashFrames();
  const auto isWindows = [&](std::size_t i) { return frames[i].may_be_dispatcher; };
  const auto handler = ee::FindHandlerFrame(kDispatcherIndex, isWindows);
  Require(handler && *handler == 7u, "the nvngx_dlssg.dll frame next above ntdll's handler call");
  Require(!ee::FindHandlerFrame(kDispatcherIndex, [](std::size_t) { return true; }), "only Windows above it");
  Require(!ee::FindHandlerFrame(0u, isWindows), "nothing above frame 0");
  Require(!ee::FindHandlerFrame(kDispatcherIndex, {}), "no test, no frame");

  EarlierExceptionInfo e{};
  e.handler_only_modules = { L"nvngx_dlssg.dll" };
  Require(!ee::IsHandlerOnlyModule(e, L"nvngx_dlssg.dll"), "only with an earlier exception");
  e.detected = true;
  Require(ee::IsHandlerOnlyModule(e, L"NVNGX_DLSSG.DLL"), "any case");
  Require(!ee::IsHandlerOnlyModule(e, L"Precision.dll") && !ee::IsHandlerOnlyModule(e, L""), "other modules");
}

EarlierExceptionInfo CrashInfo()
{
  EarlierExceptionInfo e{};
  e.detected = true;
  e.code = 0xC0000005u;
  e.address = kFirstFault;
  e.info = { 0u, kAccessedAddress };
  e.module_filename = L"Precision.dll";
  e.module_offset = 0xE5547u;
  e.module_plus_offset = L"Precision.dll+0xe5547";
  e.module_is_mod = true;
  e.handler_module_filename = L"nvngx_dlssg.dll";
  return e;
}

void TestCodeNames()
{
  Require(ee::CodeHex(0xC0000374u) == L"0xC0000374", "hex code");
  Require(ee::CodeName(0xC0000374u, true) == L"heap corruption" && ee::CodeName(0xC0000374u, false) == L"힙 손상",
    "heap corruption");
  Require(ee::CodeName(0xC0000005u, true) == L"access violation", "access violation");
  Require(ee::CodeName(0xC00000FDu, false) == L"스택 오버플로", "stack overflow");
  Require(ee::CodeName(0x12345678u, true).empty(), "no name for an unknown code");
  Require(ee::IsErrorCode(0xC0000005u) && ee::IsErrorCode(0xE06D7363u) && !ee::IsErrorCode(0x80000003u),
    "error severity");
}

void TestDescriptions()
{
  const auto e = CrashInfo();
  const auto en = ee::DescribeTechnical(e, 0xC0000374u, true);
  Require(en == L"A memory read error (access violation 0xC0000005, address 0x1DFD5DF3000) happened first at "
                L"Precision.dll+0xe5547, and the reported exception (0xC0000374, heap corruption) happened inside "
                L"the exception handler of nvngx_dlssg.dll while it was handling that error.",
    "English sentence");
  const auto ko = ee::DescribeTechnical(e, 0xC0000374u, false);
  Require(ko == L"Precision.dll+0xe5547에서 메모리 읽기 오류(0xC0000005, 주소 0x1DFD5DF3000)가 먼저 났고, "
                L"보고된 예외(0xC0000374, 힙 손상)는 그 오류를 처리하던 nvngx_dlssg.dll의 예외 처리기 안에서 "
                L"났습니다.",
    "Korean sentence");

  Require(ee::DescribePlain(e, 0xC0000374u, true) ==
            L"An error reading memory happened first in Precision.dll, and the game then crashed inside "
            L"nvngx_dlssg.dll while that error was being handled (heap corruption).",
    "English plain line");
  Require(ee::DescribePlain(e, 0xC0000374u, false) ==
            L"Precision.dll에서 메모리를 읽다가 먼저 오류가 났고, 게임은 그 오류를 처리하던 nvngx_dlssg.dll 안에서 "
            L"튕겼습니다(힙 손상).",
    "Korean plain line");

  auto write = e;
  write.info = { 1u, 0x10u };
  Require(Contains(ee::DescribeTechnical(write, 0xC0000374u, false), L"메모리 쓰기 오류(0xC0000005, 주소 0x10)"),
    "a write");
  Require(Contains(ee::DescribePlain(write, 0xC0000374u, true), L"An error writing memory"), "a write, plain");
  auto execute = e;
  execute.info = { 8u, kFirstFault };
  Require(Contains(ee::DescribeTechnical(execute, 0xC0000374u, true),
            L"An error running code at a bad address (access violation 0xC0000005, address 0x7FFBD2A55547)"),
    "an execute");
  auto generalProtection = e;
  generalProtection.info = { 0u, ~std::uint64_t{ 0 } };
  Require(Contains(ee::DescribeTechnical(generalProtection, 0xC0000374u, false), L"메모리 읽기 오류(0xC0000005)가") &&
            Contains(ee::DescribeTechnical(generalProtection, 0xC0000374u, true), L"(access violation 0xC0000005) happened"),
    "a general protection fault names no address");
  Require(ee::DescribePlain(e, 0xC0000025u, false).find(L"(계속할 수 없는 예외)") != std::wstring::npos,
    "codes a broken handler raises have a name");
  auto other = e;
  other.code = 0xC000001Du;
  other.info.clear();
  Require(Contains(ee::DescribeTechnical(other, 0xC0000374u, false), L"에서 예외(0xC000001D, 잘못된 명령어)가 먼저 났고"),
    "another code with a name");
  Require(Contains(ee::DescribePlain(other, 0xC0000374u, true), L"An error (illegal instruction) happened first in"),
    "another code, plain");
  auto unnamed = e;
  unnamed.code = 0xC0001234u;
  unnamed.info.clear();
  Require(Contains(ee::DescribeTechnical(unnamed, 0xE0001234u, true), L"An exception (0xC0001234) happened first") &&
            Contains(ee::DescribeTechnical(unnamed, 0xE0001234u, true), L"the reported exception (0xE0001234) happened"),
    "codes without a name");

  auto same = e;
  same.handler_module_filename = L"PRECISION.DLL";
  Require(Contains(ee::DescribeTechnical(same, 0xC0000374u, true), L"inside an exception handler in the same file"),
    "its own handler");
  Require(Contains(ee::DescribeTechnical(same, 0xC0000374u, false), L"같은 파일의 예외 처리기 안에서"), "its own handler, ko");
  Require(Contains(ee::DescribePlain(same, 0xC0000374u, false), L"그 오류를 처리하던 같은 파일 안에서"), "its own, plain");
  auto none = e;
  none.handler_module_filename.clear();
  Require(Contains(ee::DescribeTechnical(none, 0xC0000374u, true), L"happened while that error was being handled."),
    "no handler module");
  Require(Contains(ee::DescribeTechnical(none, 0xC0000374u, false), L"그 오류를 처리하던 중에 났습니다."),
    "no handler module, ko");
  Require(Contains(ee::DescribePlain(none, 0xC0000374u, false), L"게임은 그 오류를 처리하던 중에 튕겼습니다(힙 손상)."),
    "no handler module, plain");
  auto nowhere = e;
  nowhere.module_filename.clear();
  nowhere.module_plus_offset.clear();
  Require(Contains(ee::DescribeTechnical(nowhere, 0xC0000374u, true), L"happened first at 0x7FFBD2A55547"),
    "an address outside every module");
  Require(Contains(ee::DescribePlain(nowhere, 0xC0000374u, false), L"0x7FFBD2A55547에서"), "outside, plain");
  Require(ee::DescribeTechnical(EarlierExceptionInfo{}, 0xC0000374u, true).empty() &&
            ee::DescribePlain(EarlierExceptionInfo{}, 0xC0000374u, false).empty(),
    "nothing without one");
}

}  // namespace

int main()
{
  struct Case
  {
    const char* name;
    void (*run)();
  };
  const Case cases[] = {
    { "FindsTheRealLayout", TestFindsTheRealLayout },
    { "RecordElsewhereInTheWindow", TestRecordElsewhereInTheWindow },
    { "NothingWithoutAMatchingContext", TestNothingWithoutAMatchingContext },
    { "NothingWithoutAMatchingRecord", TestNothingWithoutAMatchingRecord },
    { "TheDeepestDispatcherWins", TestTheDeepestDispatcherWins },
    { "DispatcherAtTheEndOfTheWalk", TestDispatcherAtTheEndOfTheWalk },
    { "TheTopFrameIsNeverTheDispatcher", TestTheTopFrameIsNeverTheDispatcher },
    { "HandlerFrame", TestHandlerFrame },
    { "CodeNames", TestCodeNames },
    { "Descriptions", TestDescriptions },
  };
  int failed = 0;
  for (const auto& c : cases) {
    try {
      c.run();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "earlier_exception_tests: %s FAIL: %s\n", c.name, e.what());
      ++failed;
    }
  }
  if (failed != 0) {
    return 1;
  }
  std::printf("earlier_exception_tests: OK\n");
  return 0;
}
