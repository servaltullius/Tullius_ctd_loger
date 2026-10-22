// shared/SkyrimDiagHangObjectThreads.h: which threads of a freeze capture the
// helper keeps object memory for and the analyzer reads objects from.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "SkyrimDiagHangObjectThreads.h"

namespace {

using skydiag::hang_objects::SelectThreadIds;
using skydiag::hang_objects::WaitChainThreadIds;
using Tids = std::vector<std::uint32_t>;

constexpr std::uint32_t kPid = 900;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string Node(std::uint32_t tid, std::uint32_t status, std::uint32_t pid = kPid)
{
  return "{\"objectType\":8,\"objectStatus\":" + std::to_string(status) + ",\"thread\":{\"processId\":" +
    std::to_string(pid) + ",\"threadId\":" + std::to_string(tid) + "}}";
}

std::string Entry(std::uint32_t tid, const std::string& nodes)
{
  return "{\"tid\":" + std::to_string(tid) + ",\"isCycle\":false,\"nodes\":[" + nodes + "]}";
}

std::string Capture(const std::string& primary, const std::string& pass0, const std::string& pass1)
{
  return "{\"pid\":" + std::to_string(kPid) + ",\"threads\":[" + primary + "],\"passes\":[" +
    "{\"pass_index\":0,\"capture_usable\":true,\"threads\":[" + pass0 + "]}," +
    "{\"pass_index\":1,\"capture_usable\":true,\"threads\":[" + pass1 + "]}]}";
}

const std::string kLock = "{\"objectType\":1,\"objectStatus\":6,\"objectName\":\"\"}";

void TestOrder()
{
  // Main 1 waits on a lock owned by 2, which waits on 3 (another process's
  // thread 77 is skipped); 10 and 11 run in both passes, 12 only in the
  // second; 11 is a game thread; 20 waits.
  const std::string mainEntry = Entry(1, Node(1, 3) + "," + kLock + "," + Node(2, 3) + "," + kLock + "," +
                                           Node(3, 3) + "," + kLock + "," + Node(77, 3, 5));
  const std::string common = mainEntry + "," + Entry(10, Node(10, 2)) + "," + Entry(11, Node(11, 2)) + "," +
    Entry(20, Node(20, 3));
  const std::string wct = Capture(common + "," + Entry(12, Node(12, 3)), common + "," + Entry(12, Node(12, 3)),
    common + "," + Entry(12, Node(12, 2)));

  Require(WaitChainThreadIds(1, wct) == Tids({ 2, 3 }), "the chain in order, this process only");
  Require(SelectThreadIds(1, wct, { 11 }) == Tids({ 1, 2, 3, 11, 10, 12 }),
    "main, chain, game threads, both passes, then one pass");
  Require(SelectThreadIds(1, wct, {}) == Tids({ 1, 2, 3, 10, 11, 12 }), "without game threads, capture order");
  Require(SelectThreadIds(1, wct, { 11 }, 3) == Tids({ 1, 2, 3 }), "the cap keeps the most important");
  Require(SelectThreadIds(0, wct, {}) == Tids({ 10, 11, 12 }), "no main thread: running threads only");
  Require(SelectThreadIds(10, wct, {}) == Tids({ 10, 11, 12 }), "the main thread is not listed twice");
}

void TestMalformedCaptures()
{
  Require(SelectThreadIds(5, "", {}) == Tids({ 5 }), "no capture: the main thread");
  Require(SelectThreadIds(5, "not json", {}) == Tids({ 5 }), "bad JSON: the main thread");
  Require(SelectThreadIds(5, "{\"threads\":7}", {}) == Tids({ 5 }), "wrong types are ignored");
  Require(SelectThreadIds(5, "{\"threads\":[{\"tid\":\"9\",\"nodes\":[{\"objectType\":8,\"objectStatus\":2}]}]}", {}) ==
      Tids({ 5 }),
    "a string tid is ignored");
  Require(WaitChainThreadIds(5, "[]").empty(), "an array is no capture");
  // No usable pass: the primary list decides.
  const std::string primaryOnly = "{\"pid\":900,\"threads\":[" + Entry(8, Node(8, 2)) +
    "],\"passes\":[{\"pass_index\":0,\"capture_usable\":false,\"threads\":[]}]}";
  Require(SelectThreadIds(0, primaryOnly, {}) == Tids({ 8 }), "the primary list without usable passes");
}

}  // namespace

int main()
{
  try {
    TestOrder();
    TestMalformedCaptures();
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "hang_object_threads_tests failed: %s\n", ex.what());
    return 1;
  }
  std::puts("hang_object_threads_tests passed");
  return 0;
}
