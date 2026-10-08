#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::helper {

// An SKSE plugin DLL whose last status in skse64.log is not "loaded correctly".
struct SkseLogPluginIssue
{
  std::string dll_name;
  std::string plugin_name;  // name from SKSEPlugin_Version; empty when the DLL has none
  std::string status;       // SKSE's own text, e.g. "disabled, incompatible with current version of the game"
  std::int64_t error_code = 0;
};

struct SkseLogSummary
{
  bool recognized = false;          // the text has the SKSE64 runtime header
  std::string skse_version;         // e.g. "2.2.6"
  std::uint64_t image_base = 0;     // game executable base of the session that wrote the log
  std::uint32_t checked_count = 0;  // DLLs SKSE examined in Data\SKSE\Plugins
  std::uint32_t loaded_count = 0;   // plugins whose last status is "loaded correctly"
  std::vector<SkseLogPluginIssue> issues;
};

// Reads the plugin results SKSE writes to skse64.log:
//   plugin <dll> (<dataVersion> <name> <pluginVersion>) loaded correctly (handle N)
//   plugin <dll> (<dataVersion> <name> <pluginVersion>) <status> <code> (handle N)
// A plugin can be reported more than once (a post-load crash follows its
// "loaded correctly" line), so its last line decides.
SkseLogSummary ParseSkseLog(std::string_view text);

}  // namespace skydiag::helper
