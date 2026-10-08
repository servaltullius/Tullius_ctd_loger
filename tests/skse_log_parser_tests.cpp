// skse64.log lines in the shapes SKSE64 2.2.x writes them (PluginManager.cpp),
// taken from a real 1.6.1170 session where 320 of 322 DLLs loaded.

#include <cassert>
#include <string>

#include "SkyrimDiagHelper/SkseLogParser.h"

using skydiag::helper::ParseSkseLog;

namespace {

constexpr char kSessionLog[] =
  "SKSE64 runtime: initialize (version = 2.2.6 01064920 01DD574A9E6B4D48, os = 6.2 (9200))\r\n"
  "imagebase = 00007FF7B4BD0000\r\n"
  "reloc mgr imagebase = 00007FF7B4BD0000\r\n"
  "config path = G:\\TAKEALOOK\\Stock Game\\Data\\SKSE\\skse.ini\r\n"
  "plugin directory = G:\\TAKEALOOK\\Stock Game\\Data\\SKSE\\Plugins\\\r\n"
  "scanning plugin directory G:\\TAKEALOOK\\Stock Game\\Data\\SKSE\\Plugins\\\r\n"
  "checking plugin DialogueMovementEnabler.dll\r\n"
  "checking plugin msdia140.dll\r\n"
  "plugin msdia140.dll (00000000  00000000) no version data 0 (handle 0)\r\n"
  "checking plugin NpcGhostFix.dll\r\n"
  "plugin NpcGhostFix.dll (00000000  00000000) no version data 0 (handle 0)\r\n"
  "checking plugin SkyrimDiskCache.dll\r\n"
  "checking plugin SmoothCam.dll\r\n"
  "loading plugin \"DialogueMovementEnabler\"\r\n"
  "plugin DialogueMovementEnabler.dll (00000001 DialogueMovementEnabler 02020010) loaded correctly (handle 65)\r\n"
  "loading plugin \"Skyrim Disk Cache Enabler\"\r\n"
  "plugin SkyrimDiskCache.dll (00000001 Skyrim Disk Cache Enabler 01000000) loaded correctly (handle 66)\r\n"
  "plugin SmoothCam.dll (00000001 SmoothCam 00000013) loaded correctly (handle 278)\r\n"
  "registering plugin listener for SmoothCam at 41 of 321\r\n";

void TestRealSessionShape()
{
  const auto log = ParseSkseLog(kSessionLog);
  assert(log.recognized);
  assert(log.skse_version == "2.2.6");
  assert(log.image_base == 0x00007FF7B4BD0000ull);
  assert(log.checked_count == 5u);
  assert(log.loaded_count == 3u);
  assert(log.issues.size() == 2u);
  assert(log.issues[0].dll_name == "msdia140.dll");
  assert(log.issues[0].plugin_name.empty());
  assert(log.issues[0].status == "no version data");
  assert(log.issues[0].error_code == 0);
  assert(log.issues[1].dll_name == "NpcGhostFix.dll");
}

void TestRejectionsAndPostLoadCrash()
{
  const auto log = ParseSkseLog(
    "SKSE64 runtime: initialize (version = 2.3.1 01065400 01DD000000000000, os = 6.2 (9200))\n"
    "imagebase = 00007FF600000000\n"
    "plugin OldPlugin.dll (00000001 Old Plugin (SE) 00010000) disabled, incompatible with current version of the game 0 (handle 0)\n"
    "plugin Missing Dep.dll (00000001 Missing Dep 00010000) couldn't load plugin 126 (handle 12)\n"
    "plugin Crashy.dll (00000001 Crashy 00010000) loaded correctly (handle 13)\n"
    "plugin Crashy.dll (00000001 Crashy 00010000) crashed during postload 0 (handle 13)\n"
    "plugin Fine.DLL (00000001 Fine 00010000) loaded correctly (handle 14)\n");
  assert(log.recognized);
  assert(log.skse_version == "2.3.1");
  assert(log.image_base == 0x00007FF600000000ull);
  assert(log.loaded_count == 1u);
  assert(log.issues.size() == 3u);
  // Names may contain spaces and parentheses.
  assert(log.issues[0].plugin_name == "Old Plugin (SE)");
  assert(log.issues[0].status == "disabled, incompatible with current version of the game");
  assert(log.issues[1].dll_name == "Missing Dep.dll");
  assert(log.issues[1].status == "couldn't load plugin");
  assert(log.issues[1].error_code == 126);
  // A crash after "loaded correctly" is the plugin's last word.
  assert(log.issues[2].dll_name == "Crashy.dll");
  assert(log.issues[2].status == "crashed during postload");
}

void TestOtherTextIsIgnored()
{
  const auto empty = ParseSkseLog("");
  assert(!empty.recognized);
  assert(empty.image_base == 0u);

  const auto noise = ParseSkseLog(
    "imagebase = not-hex\n"
    "plugin directory = C:\\Game\\Data\\SKSE\\Plugins\\\n"
    "plugin Broken.dll (0000001 Short 00010000) loaded correctly (handle 1)\n"
    "plugin Broken2.dll (00000001 NoClose 00010000 loaded correctly (handle 1)\n"
    "plugin Broken3.dll (00000001 NoHandle 00010000) disabled, bad version data 0\n"
    "plugin Broken4.dll (00000001 NoCode 00010000) disabled, bad version data (handle 1)\n"
    "plugin .dll (00000001 x 00010000) loaded correctly (handle 1)\n"
    "plugin X.dll (00000001 x 00010000) \n"
    "plugin X.dll (00000001 x 00010000) )\n"
    "plugin X.dll (\n");
  assert(!noise.recognized);
  assert(noise.image_base == 0u);
  assert(noise.loaded_count == 0u);
  assert(noise.issues.empty());
}

}  // namespace

int main()
{
  TestRealSessionShape();
  TestRejectionsAndPostLoadCrash();
  TestOtherTextIsIgnored();
  return 0;
}
