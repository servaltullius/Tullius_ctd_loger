// ADR-0025 (2026-11-06): the helper follows the blackbox for saves that
// finished loading and keeps them in loaded_saves.json for the analyzer.

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "HelperRuntimeTestUtils.h"
#include "SaveLoadRecord.h"
#include "SkyrimDiagSaveLoad.h"
#include "SkyrimDiagShared.h"

using skydiag::helper::internal::LoadedSave;
using skydiag::helper::internal::MergeLoadedSavesJson;
using skydiag::helper::internal::RecordLoadedSaves;
using skydiag::helper::internal::SaveLoadWatcher;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::Require;

namespace {

constexpr std::uint32_t kPid = 0x1234u;
constexpr std::uint64_t kStartQpc = 0x5000u;
constexpr std::uint64_t kQpcPerSecond = 1000u;
constexpr std::string_view kSave75 = "Save75_FC70D793_0_646F726F_Tamriel_000839_20261101064910_14_1";
constexpr std::string_view kSave76 = "Save76_FC70D793_0_646F726F_Tamriel_000843_20261101071101_14_1";

struct Ring
{
  std::unique_ptr<skydiag::SharedLayout> shm = std::make_unique<skydiag::SharedLayout>();

  Ring()
  {
    shm->header.pid = kPid;
    shm->header.start_qpc = kStartQpc;
    shm->header.qpc_freq = kQpcPerSecond;
  }

  // Claims the next index; Commit writes it as the plugin does (seq i*2).
  std::uint32_t Claim()
  {
    const std::uint32_t i = shm->header.write_index;
    shm->header.write_index = i + 1u;
    return i;
  }

  void Commit(std::uint32_t i, skydiag::EventType type, std::uint64_t qpc, skydiag::EventPayload payload = {})
  {
    auto& e = shm->events[i % skydiag::kEventCapacity];
    e.type = static_cast<std::uint16_t>(type);
    e.qpc = qpc;
    e.payload = payload;
    e.seq = i * 2u;
  }

  void Add(skydiag::EventType type, std::uint64_t qpc, skydiag::EventPayload payload = {})
  {
    Commit(Claim(), type, qpc, payload);
  }

  void Start(std::string_view name, std::uint64_t qpc)
  {
    const auto words = skydiag::MakeSaveLoadStartPayload(name);
    Add(skydiag::EventType::kSaveLoadStart, qpc, { words.a, words.b, words.c, words.d });
  }

  void End(skydiag::SaveLoadResult result, std::uint64_t qpc)
  {
    Add(skydiag::EventType::kSaveLoadEnd, qpc, { static_cast<std::uint64_t>(result), 0, 0, 0 });
  }
};

void TestFinishedLoadsAreFound()
{
  Ring ring;
  ring.Add(skydiag::EventType::kHeartbeat, kStartQpc + 100u);
  ring.Start(kSave75, kStartQpc + 1000u);
  const std::uint32_t startEvent = ring.shm->header.write_index - 1u;
  ring.End(skydiag::SaveLoadResult::kLoaded, kStartQpc + 9000u);

  SaveLoadWatcher watcher;
  // Now is 1 s after the load finished.
  auto loads = watcher.Poll(ring.shm.get(), kStartQpc + 10000u, 1'800'000'000u);
  Require(loads.size() == 1u, "one finished load");
  Require(loads[0].key == skydiag::SaveNameKey(kSave75) && loads[0].name == "Save75_FC70D793_0_646F7" &&
            loads[0].attempt_key == skydiag::SaveLoadAttemptKey(kPid, kStartQpc, startEvent) &&
            loads[0].loaded_unix == 1'800'000'000u - 1u,
    "its key, name, attempt and time");
  Require(watcher.Poll(ring.shm.get(), kStartQpc + 11000u, 1'800'000'001u).empty(), "read once");

  // A failed load, a new game, and a load still going are no finished load.
  ring.Start(kSave76, kStartQpc + 20000u);
  ring.End(skydiag::SaveLoadResult::kFailed, kStartQpc + 21000u);
  ring.Start(kSave76, kStartQpc + 22000u);
  ring.End(skydiag::SaveLoadResult::kNewGame, kStartQpc + 23000u);
  ring.Start(kSave76, kStartQpc + 24000u);
  Require(watcher.Poll(ring.shm.get(), kStartQpc + 25000u, 1'800'000'015u).empty(), "failed, new game, in progress");
  ring.End(skydiag::SaveLoadResult::kLoaded, kStartQpc + 26000u);
  loads = watcher.Poll(ring.shm.get(), kStartQpc + 26000u, 1'800'000'016u);
  Require(loads.size() == 1u && loads[0].key == skydiag::SaveNameKey(kSave76), "the load that went on finished");

  // An unnamed load has no key to tell saves apart.
  ring.Start("", kStartQpc + 27000u);
  ring.End(skydiag::SaveLoadResult::kLoaded, kStartQpc + 28000u);
  Require(watcher.Poll(ring.shm.get(), kStartQpc + 28000u, 1'800'000'018u).empty(), "an unnamed load");

  // A helper started again reads the ring from its start: the same attempts.
  SaveLoadWatcher again;
  loads = again.Poll(ring.shm.get(), kStartQpc + 28000u, 1'800'000'018u);
  Require(loads.size() == 2u && loads[0].attempt_key == skydiag::SaveLoadAttemptKey(kPid, kStartQpc, startEvent),
    "a new watcher finds the session's loads again");
}

void TestUnwrittenEntryIsReadLater()
{
  Ring ring;
  ring.Start(kSave75, kStartQpc + 1000u);
  // The plugin claimed the end's index but has not written it yet.
  const auto end = ring.Claim();
  SaveLoadWatcher watcher;
  Require(watcher.Poll(ring.shm.get(), kStartQpc + 2000u, 1'800'000'000u).empty(), "nothing finished yet");
  ring.Commit(end, skydiag::EventType::kSaveLoadEnd, kStartQpc + 3000u,
    { static_cast<std::uint64_t>(skydiag::SaveLoadResult::kLoaded), 0, 0, 0 });
  const auto loads = watcher.Poll(ring.shm.get(), kStartQpc + 3000u, 1'800'000'001u);
  Require(loads.size() == 1u, "the entry is read once it is written");

  // A slot a later lap took is skipped, not read as this index.
  Ring lapped;
  lapped.Start(kSave75, kStartQpc + 1000u);
  lapped.shm->events[1].seq = (1u + skydiag::kEventCapacity) * 2u;
  lapped.shm->header.write_index = 2u;
  Require(SaveLoadWatcher{}.Poll(lapped.shm.get(), kStartQpc + 2000u, 1'800'000'000u).empty(), "a lapped slot");
}

LoadedSave Load(std::string_view name, std::string attempt, std::uint64_t unix)
{
  LoadedSave load{};
  load.key = skydiag::SaveNameKey(name);
  load.name = std::string(name.substr(0, 6));
  load.attempt_key = std::move(attempt);
  load.loaded_unix = unix;
  return load;
}

void TestFileKeepsEachLoadOnce()
{
  auto text = MergeLoadedSavesJson("", { Load(kSave75, "1234-5000-1", 100u) });
  text = MergeLoadedSavesJson(text, { Load(kSave75, "1234-5000-1", 100u), Load(kSave76, "1234-5000-7", 200u) });
  auto j = nlohmann::json::parse(text);
  Require(j["version"] == 1 && j["loads"].size() == 2u && j["loads"][0]["save_name"] == "Save75" &&
            j["loads"][0]["save_key"] == skydiag::SaveKeyHex(skydiag::SaveNameKey(kSave75)) &&
            j["loads"][1]["loaded_unix"] == 200 && j["loads"][1]["attempt_key"] == "1234-5000-7",
    "one row per attempt, oldest first");

  std::vector<LoadedSave> many;
  for (std::size_t i = 0; i < skydiag::kLoadedSavesKept + 5u; ++i) {
    many.push_back(Load(kSave75, "1234-5000-" + std::to_string(i + 100u), 1000u + i));
  }
  j = nlohmann::json::parse(MergeLoadedSavesJson(text, many));
  Require(j["loads"].size() == skydiag::kLoadedSavesKept && j["loads"].back()["loaded_unix"] == 1000u + 68u &&
            j["loads"].front()["loaded_unix"] == 1000u + 69u - skydiag::kLoadedSavesKept,
    "only the newest are kept");

  for (const std::string_view foreign : { "{ damaged", R"({"version":2,"loads":[]})", R"({"loads":"x"})" }) {
    j = nlohmann::json::parse(MergeLoadedSavesJson(std::string(foreign), { Load(kSave76, "1-1-1", 5u) }));
    Require(j["version"] == 1 && j["loads"].size() == 1u, "a damaged or foreign file starts over");
  }
}

void TestRecordWritesTheFile()
{
  const auto dir = MakeTempDir(L"skydiag_loaded_saves");
  Require(RecordLoadedSaves(dir, {}), "nothing to add");
  Require(!std::filesystem::exists(dir / skydiag::kLoadedSavesFileName), "no file for nothing");
  Require(RecordLoadedSaves(dir, { Load(kSave75, "1234-5000-1", 100u) }), "first load");
  Require(RecordLoadedSaves(dir, { Load(kSave75, "1234-5000-1", 100u), Load(kSave76, "1234-5000-7", 200u) }),
    "the same load again and a new one");
  std::string text;
  Require(skydiag::helper::internal::ReadTextFileUtf8(dir / skydiag::kLoadedSavesFileName, &text), "read back");
  Require(nlohmann::json::parse(text)["loads"].size() == 2u, "two loads, each once");
  Require(skydiag::helper::internal::LoadedSaveLogLine(Load(kSave75, "1-1-1", 1u), true) ==
              L"Save load finished: Save75" &&
            skydiag::helper::internal::LoadedSaveLogLine(Load(kSave75, "1-1-1", 1u), false) ==
              L"Save load finished: Save75 (loaded_saves.json could not be written)",
    "the helper log line");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main()
{
  try {
    SKYDIAG_RUN_TEST(TestFinishedLoadsAreFound);
    SKYDIAG_RUN_TEST(TestUnwrittenEntryIsReadLater);
    SKYDIAG_RUN_TEST(TestFileKeepsEachLoadOnce);
    SKYDIAG_RUN_TEST(TestRecordWritesTheFile);
    std::puts("save load record tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
