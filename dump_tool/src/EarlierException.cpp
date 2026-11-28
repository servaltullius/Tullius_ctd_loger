#include "EarlierException.h"

#include <cstring>
#include <cwchar>
#include <cwctype>

namespace skydiag::dump_tool::earlier_exception {
namespace {

constexpr std::uint32_t kAccessViolation = 0xC0000005u;

std::uint32_t Load32(const std::uint8_t* p)
{
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::uint64_t Load64(const std::uint8_t* p)
{
  std::uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::wstring Hex64(std::uint64_t v)
{
  wchar_t buf[32]{};
  std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

bool SameFileName(std::wstring_view a, std::wstring_view b)
{
  if (a.size() != b.size() || a.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i])) {
      return false;
    }
  }
  return true;
}

// The record of an error exception at rip, read from the window after the
// CONTEXT at contextAddress; nullopt when the window holds none. The first
// record at rip is the one dispatched: when it is no error (a breakpoint, a
// guard page), what follows it is not looked at.
std::optional<Record> FindRecordAfter(std::uint64_t contextAddress, std::uint64_t rip, const ReadMemory& read)
{
  // A corrupt dump's stack pointer near the top of the address space must
  // not wrap the window around (the caller checks contextAddress).
  const std::uint64_t from = contextAddress + kContextSize;
  for (std::size_t offset = 0; offset + kRecordSize <= kRecordSearchBytes; offset += 8u) {
    const std::uint64_t at = from + offset;
    std::uint8_t head[kRecordParametersOffset]{};
    if (!read(at, head, sizeof(head))) {
      continue;
    }
    const std::uint32_t code = Load32(head);
    const std::uint32_t count = Load32(head + kRecordParameterCountOffset);
    if (Load64(head + kRecordAddressOffset) != rip || count > kMaxParameters) {
      continue;
    }
    if (!IsErrorCode(code)) {
      return std::nullopt;
    }
    Record record{};
    record.record_address = at;
    record.code = code;
    record.flags = Load32(head + 4u);
    record.address = rip;
    if (count != 0u) {
      std::vector<std::uint8_t> params(static_cast<std::size_t>(count) * 8u);
      if (!read(at + kRecordParametersOffset, params.data(), params.size())) {
        continue;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        record.info.push_back(Load64(params.data() + static_cast<std::size_t>(i) * 8u));
      }
    }
    return record;
  }
  return std::nullopt;
}

// "read", "write", "execute" of an access violation; empty for anything else.
std::wstring_view AccessKind(const EarlierExceptionInfo& e)
{
  if (e.code != kAccessViolation || e.info.size() < 2u) {
    return {};
  }
  switch (e.info[0]) {
    case 0: return L"read";
    case 1: return L"write";
    case 8: return L"execute";
    default: return {};
  }
}

// Where the earlier exception happened: "Precision.dll+0xe5547", or the address.
std::wstring Place(const EarlierExceptionInfo& e)
{
  return !e.module_plus_offset.empty() ? e.module_plus_offset : Hex64(e.address);
}

std::wstring File(const EarlierExceptionInfo& e)
{
  return !e.module_filename.empty() ? e.module_filename : Hex64(e.address);
}

// "(0xC0000374, heap corruption)" without the parentheses.
std::wstring CodeWithName(std::uint32_t code, bool en)
{
  const auto name = CodeName(code, en);
  return name.empty() ? CodeHex(code) : CodeHex(code) + L", " + name;
}

enum class Handler : std::uint8_t { kNone, kSameFile, kOther };

Handler HandlerOf(const EarlierExceptionInfo& e)
{
  if (e.handler_module_filename.empty()) {
    return Handler::kNone;
  }
  return SameFileName(e.handler_module_filename, e.module_filename) ? Handler::kSameFile : Handler::kOther;
}

}  // namespace

bool IsErrorCode(std::uint32_t code)
{
  return (code & 0xC0000000u) == 0xC0000000u;
}

std::optional<Record> FindEarliest(const std::vector<Frame>& frames, const ReadMemory& read)
{
  if (!read || frames.size() < 2u) {
    return std::nullopt;
  }
  // From the bottom: the deepest dispatcher dispatched the first exception.
  for (std::size_t i = frames.size() - 1u; i >= 1u; --i) {
    const auto& frame = frames[i];
    if (!frame.may_be_dispatcher || frame.sp == 0u || frame.sp > kMaxUserAddress) {
      continue;
    }
    std::uint8_t context[kContextRipOffset + 8u]{};
    if (!read(frame.sp, context, sizeof(context))) {
      continue;
    }
    std::uint16_t segCs = 0;
    std::memcpy(&segCs, context + kContextSegCsOffset, sizeof(segCs));
    const std::uint32_t flags = Load32(context + kContextFlagsOffset);
    const std::uint64_t rsp = Load64(context + kContextRspOffset);
    const std::uint64_t rip = Load64(context + kContextRipOffset);
    const bool hasNext = i + 1u < frames.size();
    // A thread interrupted in 64-bit user mode: its code segment, and a Rip
    // and Rsp in user space above the dispatcher's frame.
    if ((flags & kContextAmd64) == 0u || (flags & kContextControlBit) == 0u || segCs != kUserCodeSegment ||
        rip < kMinUserAddress || rip > kMaxUserAddress || rsp <= frame.sp || rsp > kMaxUserAddress ||
        (hasNext && rip != frames[i + 1u].pc)) {
      continue;
    }
    if (auto record = FindRecordAfter(frame.sp, rip, read)) {
      record->dispatcher_index = i;
      record->context_address = frame.sp;
      return record;
    }
  }
  return std::nullopt;
}

bool IsHandlerOnlyModule(const EarlierExceptionInfo& earlier, std::wstring_view module)
{
  if (!earlier.detected) {
    return false;
  }
  for (const auto& handlerOnly : earlier.handler_only_modules) {
    if (SameFileName(handlerOnly, module)) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t> FindHandlerFrame(
  std::size_t dispatcherIndex,
  const std::function<bool(std::size_t frameIndex)>& passOver)
{
  if (!passOver) {
    return std::nullopt;
  }
  for (std::size_t i = dispatcherIndex; i > 0u; --i) {
    if (!passOver(i - 1u)) {
      return i - 1u;
    }
  }
  return std::nullopt;
}

std::wstring CodeName(std::uint32_t code, bool en)
{
  switch (code) {
    case 0xC0000005u: return en ? L"access violation" : L"메모리 접근 위반";
    case 0xC0000006u: return en ? L"in-page error" : L"페이지 읽기 오류";
    case 0xC000001Du: return en ? L"illegal instruction" : L"잘못된 명령어";
    case 0xC0000094u: return en ? L"integer division by zero" : L"정수 0으로 나누기";
    case 0xC0000096u: return en ? L"privileged instruction" : L"특권 명령어";
    case 0xC00000FDu: return en ? L"stack overflow" : L"스택 오버플로";
    case 0xC0000374u: return en ? L"heap corruption" : L"힙 손상";
    case 0xC0000409u: return en ? L"fail-fast" : L"즉시 종료(fail-fast)";
    case 0xC0000602u: return en ? L"fail-fast" : L"즉시 종료(fail-fast)";
    case 0xC0000008u: return en ? L"invalid handle" : L"잘못된 핸들";
    case 0xC0000017u: return en ? L"out of memory" : L"메모리 부족";
    // What Windows raises when a handler returns wrongly or continues an
    // exception that cannot be continued: typical inside a broken handler.
    case 0xC0000025u: return en ? L"noncontinuable exception" : L"계속할 수 없는 예외";
    case 0xC0000026u: return en ? L"invalid exception disposition" : L"잘못된 예외 처리 결과";
    case 0xC000041Du: return en ? L"unhandled exception in a callback" : L"콜백 안의 처리되지 않은 예외";
    case 0xC0000420u: return en ? L"assertion failure" : L"어설션 실패";
    case 0xE06D7363u: return en ? L"C++ exception" : L"C++ 예외";
    case 0x80000003u: return en ? L"breakpoint" : L"중단점";
    default: return {};
  }
}

std::wstring CodeHex(std::uint32_t code)
{
  wchar_t buf[16]{};
  std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"0x%08X", static_cast<unsigned>(code));
  return buf;
}

std::wstring DescribeTechnical(const EarlierExceptionInfo& earlier, std::uint32_t reportedCode, bool en)
{
  if (!earlier.detected) {
    return {};
  }
  const auto kind = AccessKind(earlier);
  const std::wstring code = CodeHex(earlier.code);
  const std::wstring handler = earlier.handler_module_filename;
  // A general protection fault (a non-canonical address) reports ~0, which
  // is no address.
  const bool hasAddress = !kind.empty() && earlier.info[1] != ~std::uint64_t{ 0 };
  const std::wstring accessed = hasAddress ? Hex64(earlier.info[1]) : std::wstring{};
  if (en) {
    std::wstring first;
    if (!kind.empty()) {
      const wchar_t* what = kind == L"read"    ? L"A memory read error"
                          : kind == L"write"   ? L"A memory write error"
                                               : L"An error running code at a bad address";
      first = std::wstring(what) + L" (access violation " + code + (hasAddress ? L", address " + accessed : L"") + L")";
    } else if (earlier.code == kAccessViolation) {
      first = L"A memory access error (access violation " + code + L")";
    } else {
      first = L"An exception (" + CodeWithName(earlier.code, true) + L")";
    }
    std::wstring where;
    switch (HandlerOf(earlier)) {
      case Handler::kOther:
        where = L"inside the exception handler of " + handler + L" while it was handling that error";
        break;
      case Handler::kSameFile:
        where = L"inside an exception handler in the same file while it was handling that error";
        break;
      default:
        where = L"while that error was being handled";
        break;
    }
    return first + L" happened first at " + Place(earlier) + L", and the reported exception (" +
           CodeWithName(reportedCode, true) + L") happened " + where + L".";
  }
  std::wstring first;
  if (!kind.empty()) {
    const wchar_t* what = kind == L"read" ? L"읽기" : kind == L"write" ? L"쓰기" : L"실행";
    first = std::wstring(L"메모리 ") + what + L" 오류(" + code + (hasAddress ? L", 주소 " + accessed : L"") + L")";
  } else if (earlier.code == kAccessViolation) {
    first = L"메모리 접근 오류(" + code + L")";
  } else {
    first = L"예외(" + CodeWithName(earlier.code, false) + L")";
  }
  std::wstring where;
  switch (HandlerOf(earlier)) {
    case Handler::kOther:
      where = L"그 오류를 처리하던 " + handler + L"의 예외 처리기 안에서";
      break;
    case Handler::kSameFile:
      where = L"그 오류를 처리하던 같은 파일의 예외 처리기 안에서";
      break;
    default:
      where = L"그 오류를 처리하던 중에";
      break;
  }
  return Place(earlier) + L"에서 " + first + L"가 먼저 났고, 보고된 예외(" + CodeWithName(reportedCode, false) +
         L")는 " + where + L" 났습니다.";
}

std::wstring DescribePlain(const EarlierExceptionInfo& earlier, std::uint32_t reportedCode, bool en)
{
  if (!earlier.detected) {
    return {};
  }
  const auto kind = AccessKind(earlier);
  const std::wstring name = CodeName(earlier.code, en);
  const std::wstring reported = CodeName(reportedCode, en).empty() ? CodeHex(reportedCode) : CodeName(reportedCode, en);
  const std::wstring handler = earlier.handler_module_filename;
  const bool inModule = !earlier.module_filename.empty();
  if (en) {
    std::wstring first;
    if (kind == L"read") {
      first = L"An error reading memory";
    } else if (kind == L"write") {
      first = L"An error writing memory";
    } else if (kind == L"execute") {
      first = L"An error running code at a bad address";
    } else if (earlier.code == kAccessViolation) {
      first = L"An error accessing memory";
    } else if (!name.empty()) {
      first = L"An error (" + name + L")";
    } else {
      first = L"An error";
    }
    std::wstring where;
    switch (HandlerOf(earlier)) {
      case Handler::kOther:
        where = L"inside " + handler + L" while that error was being handled";
        break;
      case Handler::kSameFile:
        where = L"inside the same file while that error was being handled";
        break;
      default:
        where = L"while that error was being handled";
        break;
    }
    return first + (inModule ? L" happened first in " : L" happened first at ") + File(earlier) +
           L", and the game then crashed " + where + L" (" + reported + L").";
  }
  std::wstring first;
  if (kind == L"read") {
    first = L"메모리를 읽다가 먼저 오류가 났고";
  } else if (kind == L"write") {
    first = L"메모리에 쓰다가 먼저 오류가 났고";
  } else if (kind == L"execute") {
    first = L"코드가 아닌 곳을 실행하다가 먼저 오류가 났고";
  } else if (earlier.code == kAccessViolation) {
    first = L"메모리를 다루다가 먼저 오류가 났고";
  } else if (!name.empty()) {
    first = L"먼저 오류(" + name + L")가 났고";
  } else {
    first = L"먼저 오류가 났고";
  }
  std::wstring where;
  switch (HandlerOf(earlier)) {
    case Handler::kOther:
      where = L"그 오류를 처리하던 " + handler + L" 안에서";
      break;
    case Handler::kSameFile:
      where = L"그 오류를 처리하던 같은 파일 안에서";
      break;
    default:
      where = L"그 오류를 처리하던 중에";
      break;
  }
  return File(earlier) + L"에서 " + first + L", 게임은 " + where + L" 튕겼습니다(" + reported + L").";
}

}  // namespace skydiag::dump_tool::earlier_exception
