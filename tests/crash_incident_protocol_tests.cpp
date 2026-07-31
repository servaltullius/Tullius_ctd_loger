#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "AnalyzerPipeline.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

namespace {

std::uint32_t ReadFlags(volatile std::uint32_t* flags) noexcept
{
  return static_cast<std::uint32_t>(InterlockedCompareExchange(
    reinterpret_cast<volatile LONG*>(flags),
    0,
    0));
}

void AcknowledgeIncidentForTest(volatile std::uint32_t* flags) noexcept
{
  InterlockedAnd(
    reinterpret_cast<volatile LONG*>(flags),
    ~static_cast<LONG>(skydiag::kState_Frozen));
}

std::vector<std::byte> BuildBlackboxMinidump(std::uint32_t protocolVersion)
{
  const std::size_t directoryOffset = sizeof(MINIDUMP_HEADER);
  const std::size_t streamOffset =
    (directoryOffset + sizeof(MINIDUMP_DIRECTORY) + alignof(skydiag::SharedLayout) - 1u) &
    ~(alignof(skydiag::SharedLayout) - 1u);
  assert(streamOffset <= static_cast<std::size_t>(std::numeric_limits<RVA>::max()));
  assert(
    sizeof(skydiag::SharedLayout) <=
    static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));

  std::vector<std::byte> dump(streamOffset + sizeof(skydiag::SharedLayout));

  MINIDUMP_HEADER header{};
  header.Signature = MINIDUMP_SIGNATURE;
  header.Version = MINIDUMP_VERSION;
  header.NumberOfStreams = 1u;
  header.StreamDirectoryRva = static_cast<RVA>(directoryOffset);
  std::memcpy(dump.data(), &header, sizeof(header));

  MINIDUMP_DIRECTORY directory{};
  directory.StreamType = skydiag::protocol::kMinidumpUserStream_Blackbox;
  directory.Location.DataSize = static_cast<ULONG>(sizeof(skydiag::SharedLayout));
  directory.Location.Rva = static_cast<RVA>(streamOffset);
  std::memcpy(dump.data() + directoryOffset, &directory, sizeof(directory));

  auto snapshot = std::make_unique<skydiag::SharedLayout>();
  snapshot->header.magic = skydiag::kMagic;
  snapshot->header.version = protocolVersion;
  snapshot->header.pid = 4242u;
  snapshot->header.capacity = 1u;
  snapshot->header.qpc_freq = 1000u;
  snapshot->header.start_qpc = 100u;
  snapshot->header.state_flags =
    skydiag::kState_Frozen | skydiag::kState_InMenu;
  snapshot->header.write_index = 1u;
  snapshot->header.crash_seq = 6u;
  snapshot->header.crash.exception_code = EXCEPTION_ACCESS_VIOLATION;
  snapshot->header.crash.faulting_tid = 77u;
  snapshot->header.crash.exception_addr = 0x12345678u;

  auto& event = snapshot->events[0];
  event.seq = 2u;
  event.tid = 77u;
  event.qpc = 150u;
  event.type = static_cast<std::uint16_t>(skydiag::EventType::kCrash);
  event.size = sizeof(skydiag::EventPayload);
  event.payload.a = EXCEPTION_ACCESS_VIOLATION;
  event.payload.b = 0x12345678u;

  std::memcpy(
    dump.data() + streamOffset,
    snapshot.get(),
    sizeof(skydiag::SharedLayout));
  return dump;
}

void VerifyOfflineBlackboxProtocolVersion(std::uint32_t protocolVersion)
{
  auto dump = BuildBlackboxMinidump(protocolVersion);
  skydiag::dump_tool::AnalysisResult result{};
  skydiag::dump_tool::ParseBlackboxStream(
    dump.data(),
    dump.size(),
    std::nullopt,
    {},
    result);

  assert(result.has_blackbox);
  assert(result.pid == 4242u);
  assert(result.state_flags == (skydiag::kState_Frozen | skydiag::kState_InMenu));
  assert(result.blackbox_crash_seq == 6u);
  assert(result.blackbox_exception_code == EXCEPTION_ACCESS_VIOLATION);
  assert(result.blackbox_faulting_tid == 77u);
  assert(result.blackbox_exception_addr == 0x12345678u);
  assert(result.events.size() == 1u);
  assert(result.events.front().type == static_cast<std::uint16_t>(skydiag::EventType::kCrash));
  assert(result.events.front().a == EXCEPTION_ACCESS_VIOLATION);
  assert(result.events.front().b == 0x12345678u);
}

void TestOfflineParserAcceptsV3AndV4OnlyAsSupportedGenerations()
{
  VerifyOfflineBlackboxProtocolVersion(3u);
  VerifyOfflineBlackboxProtocolVersion(skydiag::kVersion);

  auto futureDump = BuildBlackboxMinidump(skydiag::kVersion + 1u);
  skydiag::dump_tool::AnalysisResult futureResult{};
  skydiag::dump_tool::ParseBlackboxStream(
    futureDump.data(),
    futureDump.size(),
    std::nullopt,
    {},
    futureResult);
  assert(!futureResult.has_blackbox);
  assert(futureResult.events.empty());
}

}  // namespace

int main()
{
  static_assert(skydiag::kVersion == 4u);

  volatile std::uint32_t flags =
    skydiag::kState_Loading | skydiag::kState_InMenu;
  std::atomic<int> winners{0};

  std::vector<std::thread> contenders;
  contenders.reserve(32);
  for (int i = 0; i < 32; ++i) {
    contenders.emplace_back([&]() {
      if (skydiag::TryClaimCrashIncidentOwnership(&flags)) {
        winners.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& contender : contenders) {
    contender.join();
  }

  assert(winners.load(std::memory_order_relaxed) == 1);
  const auto claimedFlags = ReadFlags(&flags);
  assert((claimedFlags & skydiag::kState_Frozen) != 0u);
  assert((claimedFlags & skydiag::kState_Loading) != 0u);
  assert((claimedFlags & skydiag::kState_InMenu) != 0u);
  assert(!skydiag::TryClaimCrashIncidentOwnership(&flags));

  // This is the same atomic ownership clear used by the helper after it
  // rejects or abandons the committed generation.
  AcknowledgeIncidentForTest(&flags);
  assert((ReadFlags(&flags) & skydiag::kState_Frozen) == 0u);
  assert(skydiag::TryClaimCrashIncidentOwnership(&flags));
  assert(!skydiag::TryClaimCrashIncidentOwnership(&flags));

  TestOfflineParserAcceptsV3AndV4OnlyAsSupportedGenerations();
  return 0;
}
