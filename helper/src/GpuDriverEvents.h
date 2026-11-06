#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::helper::internal {

// ADR-0026: a graphics driver event from the Windows System log. A GPU fault
// or a driver reset (TDR) can leave the game waiting on the GPU forever with
// nothing in any game log, so a freeze capture keeps the session's driver
// errors beside the dump.
struct GpuDriverEvent
{
  std::wstring provider;  // nvlddmkm, amdkmdag, igfx, ..., Display (TDR 4101)
  std::uint32_t id = 0;
  std::uint32_t level = 0;  // 1 critical, 2 error, 3 warning, 4 information
  std::uint64_t unix = 0;
  std::wstring message;  // the event's own text, else its data strings
};

struct GpuDriverEventLog
{
  bool read = false;  // the System log could be queried
  std::uint64_t since_unix = 0;
  std::uint64_t until_unix = 0;
  std::size_t total = 0;  // events in the window; `events` holds the newest of them
  std::vector<GpuDriverEvent> events;  // oldest first
};

inline constexpr std::size_t kGpuDriverEventsKept = 32;

// The System log query for the display drivers' events in [since, until].
std::wstring GpuDriverEventQuery(std::uint64_t sinceUnix, std::uint64_t untilUnix);

// One event as EvtRender writes it (EvtRenderEventXml); its message is the
// data strings, a device path left out. None when the XML is not an event.
std::optional<GpuDriverEvent> ParseGpuDriverEventXml(std::wstring_view xml);

// The events in the window, newest kGpuDriverEventsKept kept; read = false
// when the log could not be queried.
GpuDriverEventLog QueryGpuDriverEvents(std::uint64_t sinceUnix, std::uint64_t untilUnix);

// The dump's user stream (kMinidumpUserStream_GpuDriverEvents); empty when
// the log was not read.
std::string GpuDriverEventsJson(const GpuDriverEventLog& log);

// When the process started, 0 when it cannot be read.
std::uint64_t ProcessStartUnix(std::uint32_t pid);

}  // namespace skydiag::helper::internal
