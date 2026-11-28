#pragma once

// ADR-0039: the exception the crashing thread was already handling when the
// reported one happened. When a hardware exception (an access violation, an
// illegal instruction) reaches user mode, the kernel saves the thread's
// CONTEXT and the EXCEPTION_RECORD on that thread's stack and runs ntdll's
// KiUserExceptionDispatcher there, which calls the vectored and frame
// handlers. A crash inside one of those handlers is reported as its own
// exception, and the stack walk from it goes through the dispatcher's frame
// down to the code that faulted first (2026-11-28: an access violation in
// Precision.dll, then heap corruption reported inside the vectored handler of
// a frame generation DLL that ran first).
//
// Pure logic, no Windows headers: it runs in the Linux tests too.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

// What the analysis found; detected is false when the crash stack holds no
// earlier exception.
struct EarlierExceptionInfo
{
  bool detected = false;
  std::uint32_t code = 0;
  std::uint32_t flags = 0;
  std::uint64_t address = 0;
  std::vector<std::uint64_t> info;  // ExceptionInformation
  std::wstring module_filename;     // empty when no loaded module holds the address
  std::wstring module_path;
  std::uint64_t module_offset = 0;
  std::wstring module_plus_offset;  // "Precision.dll+0xe5547"
  std::wstring inferred_mod_name;
  bool module_is_mod = false;  // not Windows, the game or external software
  // The nearest module above the dispatcher's frame that is not Windows: the
  // one whose handler was running (empty when only Windows code ran there).
  std::wstring handler_module_filename;
  std::wstring handler_inferred_mod_name;
  // Modules that are not Windows with frames only above the dispatcher (the
  // handler's side, where the reported exception happened): a paired Crash
  // Logger log, which records the reported exception, names them first.
  std::vector<std::wstring> handler_only_modules;
  // The earlier exception's own call stack from its faulting frame, formatted
  // as the crash stack is (the shown crash stack may end before it).
  std::vector<std::wstring> frames;
  std::uint32_t dispatcher_depth = 0;  // the dispatcher's frame in the crash stack
  std::uint64_t context_address = 0;   // where the saved CONTEXT lies on the stack
  std::uint64_t record_address = 0;    // and the EXCEPTION_RECORD
};

namespace earlier_exception {

// x64 layouts from winnt.h, fixed by the ABI.
inline constexpr std::size_t kContextSize = 0x4D0;
inline constexpr std::size_t kContextFlagsOffset = 0x30;
inline constexpr std::size_t kContextSegCsOffset = 0x38;
inline constexpr std::size_t kContextRspOffset = 0x98;
inline constexpr std::size_t kContextRipOffset = 0xF8;
inline constexpr std::uint32_t kContextAmd64 = 0x00100000u;
inline constexpr std::uint32_t kContextControlBit = 0x00000001u;  // CONTEXT_CONTROL without CONTEXT_AMD64
// The code segment of 64-bit user mode (KGDT64_R3_CODE with RPL 3), and the
// user address range: an exception the kernel hands to user mode was taken
// there.
inline constexpr std::uint16_t kUserCodeSegment = 0x33u;
inline constexpr std::uint64_t kMinUserAddress = 0x10000ull;
inline constexpr std::uint64_t kMaxUserAddress = 0x00007FFFFFFFFFFFull;
inline constexpr std::size_t kRecordSize = 0x98;                  // EXCEPTION_RECORD64
inline constexpr std::size_t kRecordAddressOffset = 0x10;
inline constexpr std::size_t kRecordParameterCountOffset = 0x18;
inline constexpr std::size_t kRecordParametersOffset = 0x20;
inline constexpr std::uint32_t kMaxParameters = 15u;
// Windows puts the record right after the CONTEXT and its CONTEXT_EX (0x4F0
// from the dispatcher's stack pointer). It is looked for in this window after
// the CONTEXT, so another extended-state layout still finds it.
inline constexpr std::size_t kRecordSearchBytes = 0x400;

struct Frame
{
  std::uint64_t pc = 0;
  std::uint64_t sp = 0;            // the frame's stack pointer from the walk
  bool may_be_dispatcher = false;  // the frame is in ntdll.dll
};

// Reads the dump's memory; false unless all size bytes were read.
using ReadMemory = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

struct Record
{
  std::size_t dispatcher_index = 0;
  std::uint64_t context_address = 0;
  std::uint64_t record_address = 0;
  std::uint32_t code = 0;
  std::uint32_t flags = 0;
  std::uint64_t address = 0;
  std::vector<std::uint64_t> info;
};

// Severity "error" (0xC0000000 and up): an access violation, an illegal
// instruction, heap corruption. Breakpoints, single steps and guard pages
// (0x80000000 range) are what debuggers and some hooks raise on purpose.
bool IsErrorCode(std::uint32_t code);

// The earliest error exception being dispatched on a walk that starts at the
// reported exception's own context. Frame i (i >= 1) is the dispatcher when
// it may be one, a CONTEXT (CONTEXT_AMD64 with CONTEXT_CONTROL, the user code
// segment) lies at its stack pointer with a Rip in user space that is frame
// i+1's pc (any such Rip for the walk's last frame) and a Rsp above it, and
// the first EXCEPTION_RECORD at that Rip, 8-byte aligned in
// kRecordSearchBytes after the CONTEXT, has an error code. With more than one
// dispatcher on the walk, the deepest one (the first exception) wins.
std::optional<Record> FindEarliest(const std::vector<Frame>& frames, const ReadMemory& read);

// True when module (a file name, any case) has frames only on the handler's
// side of the dispatcher.
bool IsHandlerOnlyModule(const EarlierExceptionInfo& earlier, std::wstring_view module);

// Index of the handler's frame: the nearest frame above the dispatcher that
// is not passed over (passOver says which: Windows code, frames outside
// every module), or nullopt.
std::optional<std::size_t> FindHandlerFrame(
  std::size_t dispatcherIndex,
  const std::function<bool(std::size_t frameIndex)>& passOver);

// A short name of an exception code ("heap corruption", "힙 손상"); empty
// for a code without one.
std::wstring CodeName(std::uint32_t code, bool en);

// "0xC0000374"
std::wstring CodeHex(std::uint32_t code);

// The report's and the conclusion's sentence: where the earlier exception
// happened and what it was, then where the reported exception happened.
// "A memory read error (access violation 0xC0000005 at 0x1DFD5DF3000)
// happened first at Precision.dll+0xe5547, and the reported exception
// (0xC0000374, heap corruption) happened inside the exception handler of
// nvngx_dlssg.dll while it was handling that error."
std::wstring DescribeTechnical(const EarlierExceptionInfo& earlier, std::uint32_t reportedCode, bool en);

// The quick answer's plain line: the same without codes and offsets.
std::wstring DescribePlain(const EarlierExceptionInfo& earlier, std::uint32_t reportedCode, bool en);

}  // namespace earlier_exception
}  // namespace skydiag::dump_tool
