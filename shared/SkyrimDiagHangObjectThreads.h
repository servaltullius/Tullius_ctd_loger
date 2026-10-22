#pragma once

// The threads of a freeze (hang) or manual capture whose registers and stack
// top are examined for game objects (ADR-0010, addendum 2026-10-22). The
// helper keeps the memory of those objects in the dump; the analyzer reads
// them back for the same threads. Shared so both sides pick the same threads,
// and free of <Windows.h> for the host-independent tests.
//
// Order, at most kMaxThreads:
//   1. the game main thread;
//   2. the threads in its WCT wait chain (lock owners, in chain order);
//   3. threads WCT saw running (not waiting) that the game itself started,
//      such as BSJobs job threads (a spinning job thread is one of these);
//   4. other threads WCT saw running.
// Within 3 and 4, threads running in the first and the last usable pass come
// before those running in only one, each in the capture's thread order.
// The WCT JSON is the helper's (helper/src/WctCapture.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::hang_objects {

inline constexpr std::size_t kMaxThreads = 16;

namespace detail {

// WCT_OBJECT_TYPE::WctThreadType and WCT_OBJECT_STATUS::WctStatusRunning (wct.h).
inline constexpr std::uint32_t kWctThreadType = 8u;
inline constexpr std::uint32_t kWctStatusRunning = 2u;

inline std::uint32_t UintField(const nlohmann::json& object, const char* key)
{
  if (!object.is_object()) {
    return 0u;
  }
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned()) {
    return 0u;
  }
  return it->get<std::uint32_t>();
}

inline const nlohmann::json* Nodes(const nlohmann::json& entry)
{
  if (!entry.is_object()) {
    return nullptr;
  }
  const auto it = entry.find("nodes");
  return (it != entry.end() && it->is_array()) ? &*it : nullptr;
}

// The thread a thread node names, 0 for any other node or a thread of
// another process.
inline std::uint32_t NodeThreadId(const nlohmann::json& node, std::uint32_t pid)
{
  if (UintField(node, "objectType") != kWctThreadType) {
    return 0u;
  }
  const auto it = node.find("thread");
  if (it == node.end() || !it->is_object()) {
    return 0u;
  }
  const std::uint32_t processId = UintField(*it, "processId");
  if (pid != 0u && processId != 0u && processId != pid) {
    return 0u;
  }
  return UintField(*it, "threadId");
}

inline bool IsRunning(const nlohmann::json& entry)
{
  const auto* nodes = Nodes(entry);
  if (!nodes || nodes->empty()) {
    return false;
  }
  const auto& self = nodes->front();
  return UintField(self, "objectType") == kWctThreadType && UintField(self, "objectStatus") == kWctStatusRunning;
}

inline std::vector<std::uint32_t> RunningThreads(const nlohmann::json& threads)
{
  std::vector<std::uint32_t> tids;
  if (!threads.is_array()) {
    return tids;
  }
  for (const auto& entry : threads) {
    const std::uint32_t tid = UintField(entry, "tid");
    if (tid != 0u && IsRunning(entry)) {
      tids.push_back(tid);
    }
  }
  return tids;
}

inline bool Contains(const std::vector<std::uint32_t>& tids, std::uint32_t tid)
{
  return std::find(tids.begin(), tids.end(), tid) != tids.end();
}

// The other threads in the main thread's chain, in chain order.
inline std::vector<std::uint32_t> WaitChain(const nlohmann::json& json, std::uint32_t mainTid)
{
  std::vector<std::uint32_t> chain;
  const auto threadsIt = json.find("threads");
  if (mainTid == 0u || threadsIt == json.end() || !threadsIt->is_array()) {
    return chain;
  }
  const std::uint32_t pid = UintField(json, "pid");
  for (const auto& entry : *threadsIt) {
    if (UintField(entry, "tid") != mainTid) {
      continue;
    }
    if (const auto* nodes = Nodes(entry)) {
      for (std::size_t i = 1; i < nodes->size(); ++i) {
        const std::uint32_t tid = NodeThreadId((*nodes)[i], pid);
        if (tid != 0u && tid != mainTid && !Contains(chain, tid)) {
          chain.push_back(tid);
        }
      }
    }
    break;
  }
  return chain;
}

}  // namespace detail

// The threads the main thread's WCT wait chain names after it (lock owners
// and the threads they wait on), in chain order.
inline std::vector<std::uint32_t> WaitChainThreadIds(std::uint32_t mainTid, std::string_view wctJsonUtf8)
{
  if (wctJsonUtf8.empty()) {
    return {};
  }
  const auto json = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/false);
  return json.is_object() ? detail::WaitChain(json, mainTid) : std::vector<std::uint32_t>{};
}

// mainTid 0: no main thread known. gameThreadTids: threads whose start
// address is in the game executable (from the live process or the dump).
inline std::vector<std::uint32_t> SelectThreadIds(
  std::uint32_t mainTid,
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& gameThreadTids,
  std::size_t maxThreads = kMaxThreads)
{
  std::vector<std::uint32_t> out;
  const auto add = [&](std::uint32_t tid) {
    if (tid != 0u && out.size() < maxThreads && !detail::Contains(out, tid)) {
      out.push_back(tid);
    }
  };
  add(mainTid);
  if (wctJsonUtf8.empty()) {
    return out;
  }
  // Parsed without exceptions, and every field is type-checked before use.
  const auto json = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object()) {
    return out;
  }
  const auto threadsIt = json.find("threads");
  const nlohmann::json empty = nlohmann::json::array();
  const nlohmann::json& primary = (threadsIt != json.end() && threadsIt->is_array()) ? *threadsIt : empty;
  for (const auto tid : detail::WaitChain(json, mainTid)) {
    add(tid);
  }

  // Running threads per usable pass; the primary list when there are none.
  std::vector<std::vector<std::uint32_t>> passes;
  if (const auto passesIt = json.find("passes"); passesIt != json.end() && passesIt->is_array()) {
    for (const auto& pass : *passesIt) {
      const auto usable = pass.is_object() ? pass.find("capture_usable") : pass.end();
      if (usable == pass.end() || !usable->is_boolean() || !usable->get<bool>()) {
        continue;
      }
      if (const auto it = pass.find("threads"); it != pass.end()) {
        passes.push_back(detail::RunningThreads(*it));
      }
    }
  }
  if (passes.empty()) {
    passes.push_back(detail::RunningThreads(primary));
  }
  std::vector<std::uint32_t> both;
  std::vector<std::uint32_t> once;
  for (const auto& pass : passes) {
    for (const auto tid : pass) {
      const bool inBoth = detail::Contains(passes.front(), tid) && detail::Contains(passes.back(), tid);
      auto& list = inBoth ? both : once;
      if (!detail::Contains(list, tid)) {
        list.push_back(tid);
      }
    }
  }
  for (const bool game : { true, false }) {
    for (const auto* list : { &both, &once }) {
      for (const auto tid : *list) {
        if (detail::Contains(gameThreadTids, tid) == game) {
          add(tid);
        }
      }
    }
  }
  return out;
}

}  // namespace skydiag::hang_objects
