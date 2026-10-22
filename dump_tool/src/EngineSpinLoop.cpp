#include "EngineSpinLoop.h"

#include <cstring>

namespace skydiag::dump_tool {
namespace {

// Length of a ModRM operand's addressing bytes after the ModRM byte (SIB and
// displacement); code[at] is the ModRM byte.
std::optional<std::size_t> ModRmTailLength(const std::vector<std::uint8_t>& code, std::size_t at)
{
  const std::uint8_t modrm = code[at];
  const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6u);
  const std::uint8_t rm = static_cast<std::uint8_t>(modrm & 7u);
  if (mod == 3u) {
    return 0u;
  }
  std::size_t tail = 0;
  if (rm == 4u) {
    if (at + 1u >= code.size()) {
      return std::nullopt;
    }
    tail = 1u;  // SIB
    if (mod == 0u && (code[at + 1u] & 7u) == 5u) {
      tail += 4u;  // SIB with no base: disp32
    }
  } else if (mod == 0u && rm == 5u) {
    tail = 4u;  // rip-relative disp32
  }
  if (mod == 1u) {
    tail += 1u;
  } else if (mod == 2u) {
    tail += 4u;
  }
  return tail;
}

// A conditional jump at code[at] taken when a compare was unordered (CF, ZF
// and PF all set): jb, jbe, je, jp, in rel8 or rel32 form. Returns the jump's
// displacement and sets end to the offset after it.
std::optional<std::int64_t> UnorderedTakenJcc(const std::vector<std::uint8_t>& code, std::size_t at, std::size_t& end)
{
  const std::size_t n = code.size();
  if (at + 2u <= n) {
    const std::uint8_t op = code[at];
    if (op == 0x72u || op == 0x76u || op == 0x74u || op == 0x7Au) {
      end = at + 2u;
      return static_cast<std::int64_t>(static_cast<std::int8_t>(code[at + 1u]));
    }
  }
  if (at + 6u <= n && code[at] == 0x0Fu) {
    const std::uint8_t op = code[at + 1u];
    if (op == 0x82u || op == 0x86u || op == 0x84u || op == 0x8Au) {
      std::int32_t rel = 0;
      std::memcpy(&rel, code.data() + at + 2u, sizeof(rel));
      end = at + 6u;
      return static_cast<std::int64_t>(rel);
    }
  }
  return std::nullopt;
}

}  // namespace

FloatCompareLoop FindFloatCompareLoop(const std::vector<std::uint8_t>& code, std::size_t ripOffset)
{
  FloatCompareLoop loop{};
  const std::size_t n = code.size();
  if (ripOffset >= n) {
    return loop;
  }
  for (std::size_t p = 0; p < n; ++p) {
    std::size_t i = p;
    const bool is66 = code[i] == 0x66u;
    if (is66) {
      ++i;
    }
    std::uint8_t rex = 0;
    if (i < n && (code[i] & 0xF0u) == 0x40u) {
      rex = code[i];
      ++i;
    }
    if (i + 3u > n || code[i] != 0x0Fu || (code[i + 1u] != 0x2Eu && code[i + 1u] != 0x2Fu)) {
      continue;
    }
    const bool unordered = code[i + 1u] == 0x2Eu;
    const std::size_t modrmAt = i + 2u;
    const auto tail = ModRmTailLength(code, modrmAt);
    if (!tail) {
      continue;
    }
    const std::size_t jccAt = modrmAt + 1u + *tail;
    std::size_t jccEnd = 0;
    const auto rel = jccAt < n ? UnorderedTakenJcc(code, jccAt, jccEnd) : std::nullopt;
    if (!rel || *rel >= 0) {
      continue;
    }
    const std::int64_t target = static_cast<std::int64_t>(jccEnd) + *rel;
    const auto rip = static_cast<std::int64_t>(ripOffset);
    // The jump goes back to the compare or before it, closes a short loop, and
    // the thread is inside that loop.
    if (target > static_cast<std::int64_t>(p) ||
        static_cast<std::int64_t>(jccEnd) - target > static_cast<std::int64_t>(kSpinLoopMaxBytes) || rip < target ||
        rip >= static_cast<std::int64_t>(jccEnd)) {
      continue;
    }
    const std::uint8_t modrm = code[modrmAt];
    loop.found = true;
    loop.is_double = is66;
    loop.unordered_compare = unordered;
    loop.reg = static_cast<std::uint8_t>(((modrm >> 3u) & 7u) | ((rex & 0x4u) != 0u ? 8u : 0u));
    loop.has_rm_reg = (modrm >> 6u) == 3u;
    loop.rm_reg = loop.has_rm_reg ? static_cast<std::uint8_t>((modrm & 7u) | ((rex & 0x1u) != 0u ? 8u : 0u)) : 0u;
    loop.compare_offset = p;
    return loop;
  }
  return loop;
}

std::string FloatCompareMnemonic(const FloatCompareLoop& loop)
{
  std::string name = loop.unordered_compare ? "ucomis" : "comis";
  name += loop.is_double ? "d" : "s";
  return name;
}

std::optional<std::uint64_t> ReadContextXmmLowLane(
  const std::uint8_t* context,
  std::size_t size,
  unsigned reg,
  bool isDouble)
{
  if (!context || reg >= kContextXmmRegisterCount) {
    return std::nullopt;
  }
  const std::size_t offset = kContextXmmRegistersOffset + static_cast<std::size_t>(reg) * 16u;
  const std::size_t width = isDouble ? 8u : 4u;
  if (size < offset || size - offset < width) {
    return std::nullopt;
  }
  if (isDouble) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, context + offset, sizeof(bits));
    return bits;
  }
  std::uint32_t bits = 0;
  std::memcpy(&bits, context + offset, sizeof(bits));
  return bits;
}

std::optional<std::uint32_t> ReadContextMxCsr(const std::uint8_t* context, std::size_t size)
{
  if (!context || size < kContextMxCsrOffset + sizeof(std::uint32_t)) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  std::memcpy(&value, context + kContextMxCsrOffset, sizeof(value));
  return value;
}

bool IsNaNBits(std::uint64_t bits, bool isDouble)
{
  if (isDouble) {
    constexpr std::uint64_t kExponent = 0x7FF0000000000000ull;
    constexpr std::uint64_t kFraction = 0x000FFFFFFFFFFFFFull;
    return (bits & kExponent) == kExponent && (bits & kFraction) != 0u;
  }
  constexpr std::uint64_t kExponent = 0x7F800000u;
  constexpr std::uint64_t kFraction = 0x007FFFFFu;
  return (bits & 0xFFFFFFFFull) == bits && (bits & kExponent) == kExponent && (bits & kFraction) != 0u;
}

std::optional<std::uint8_t> FindNaNCompareRegister(
  const FloatCompareLoop& loop,
  const std::uint8_t* context,
  std::size_t size,
  std::uint64_t* outBits)
{
  if (!loop.found) {
    return std::nullopt;
  }
  const auto check = [&](std::uint8_t reg) -> bool {
    const auto bits = ReadContextXmmLowLane(context, size, reg, loop.is_double);
    if (!bits || !IsNaNBits(*bits, loop.is_double)) {
      return false;
    }
    if (outBits) {
      *outBits = *bits;
    }
    return true;
  };
  if (check(loop.reg)) {
    return loop.reg;
  }
  if (loop.has_rm_reg && check(loop.rm_reg)) {
    return loop.rm_reg;
  }
  return std::nullopt;
}

}  // namespace skydiag::dump_tool
