#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "SkyrimDiagHelper/CrashRecapturePolicy.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"

namespace skydiag::helper {
struct AttachedProcess;
struct HangDecision;
struct HelperConfig;
}

namespace skydiag::helper::internal {

// What a hang capture and a manual capture differ in for the shared part.
struct FreezeCaptureRequest
{
  // capture.kind in the WCT file: "hang" or "manual".
  const char* kind = "";
  // "Hang" / "Manual" in the log lines ("Hang dump failed", "for hang dump").
  std::wstring_view logName;
  skydiag::helper::CaptureKind profileKind = skydiag::helper::CaptureKind::Hang;
  std::filesystem::path wctPath;
  std::wstring dumpPath;
  // A hang records the visible dialog windows (a plugin's error MessageBox).
  bool captureModalDialogs = false;
};

struct FreezeCaptureResult
{
  bool dumpWritten = false;
  std::size_t modalDialogCount = 0;
  bool pssRequested = false;
  bool pssUsed = false;
  std::uint32_t pssCaptureMs = 0;
  std::wstring pssStatus;
  skydiag::helper::DumpProfile dumpProfile{};
  skydiag::helper::DumpObjectMemoryStats objectMemory{};
  skydiag::helper::RecaptureDecision recapture{};
};

// The part a hang capture and a manual capture share, in this order: the WCT
// wait chains (and the dialog windows when asked), the plugin scan, a PSS
// snapshot when enabled, the freeze recapture decision, the WCT file, then the
// dump from the snapshot or the live process. Logs a failed WCT or dump; the
// caller logs a written dump and does what follows it.
FreezeCaptureResult CaptureFreezeDumpAndWct(
  const skydiag::helper::HelperConfig& cfg,
  const skydiag::helper::AttachedProcess& proc,
  const std::filesystem::path& outBase,
  const skydiag::helper::HangDecision& decision,
  std::uint32_t stateFlags,
  const FreezeCaptureRequest& request);

// The incident manifest context both write: the hang decision and how the dump
// was taken.
nlohmann::json FreezeManifestContext(
  const skydiag::helper::HangDecision& decision,
  bool inMenu,
  const FreezeCaptureResult& capture);

}  // namespace skydiag::helper::internal
