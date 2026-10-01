#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include "Analyzer.h"
#include "HangCaptureInternal.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "RetentionWorker.h"
#include "SkyrimDiagHelper/ModalDialogProbe.h"

using skydiag::helper::HangDecision;
using skydiag::helper::internal::ClearLog;
using skydiag::helper::internal::ExecuteConfirmedHangCapture;
using skydiag::helper::internal::HangCaptureState;
using skydiag::helper::internal::ShutdownRetentionWorker;
using skydiag::tests::runtime::CloseAttachedProcess;
using skydiag::tests::runtime::FindSingleFileWithExt;
using skydiag::tests::runtime::MakeSelfAttachedProcess;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::MakeTestConfig;
using skydiag::tests::runtime::Require;

namespace {

constexpr wchar_t kDialogTitle[] = L"SkyrimDiag modal hang e2e";

bool WaitForDialogOwnedBy(DWORD tid)
{
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (const auto& dialog : skydiag::helper::CaptureModalDialogs(GetCurrentProcessId())) {
      if (dialog.tid == tid && dialog.title == "SkyrimDiag modal hang e2e") {
        return true;
      }
    }
    Sleep(50);
  }
  return false;
}

// Captures a real hang dump while a thread that the blackbox names as the game
// main thread is parked in MessageBoxW, then runs the production analyzer on it.
void TestModalDialogHangIsClassifiedAndAttributedToDialogCaller()
{
  const auto outBase = MakeTempDir(L"skydiag_modal_dialog_hang_e2e");
  ClearLog(outBase);

  DWORD dialogTid = 0;
  HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(started != nullptr, "CreateEventW failed");
  std::thread dialogThread([&]() {
    dialogTid = GetCurrentThreadId();
    SetEvent(started);
    MessageBoxW(nullptr, L"An SKSE plugin reported a fatal load error.", kDialogTitle, MB_OK | MB_ICONERROR);
  });
  WaitForSingleObject(started, 5000);
  CloseHandle(started);
  const bool dialogVisible = WaitForDialogOwnedBy(dialogTid);

  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  auto& heartbeat = shared->events[0];
  heartbeat.tid = dialogTid;
  heartbeat.qpc = 2;
  heartbeat.type = static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat);
  heartbeat.seq = 2;
  shared->header.write_index = 1;

  auto proc = MakeSelfAttachedProcess(shared.get());
  HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10;
  std::wstring pendingViewer;
  HangCaptureState state{};
  if (dialogVisible) {
    ExecuteConfirmedHangCapture(MakeTestConfig(), proc, outBase, decision, 0u, &pendingViewer, &state);
  }

  if (HWND hwnd = FindWindowW(L"#32770", kDialogTitle); hwnd != nullptr) {
    PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
  }
  dialogThread.join();
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);

  Require(dialogVisible, "Test MessageBox never became visible");
  const auto dumpPath = FindSingleFileWithExt(outBase, L".dmp");
  Require(std::filesystem::exists(dumpPath), "Hang capture must write a dump");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the modal hang dump");

  const auto& modal = result.modal_dialog_wait;
  Require(modal.detected, "Modal dialog wait must be detected");
  Require(modal.main_thread_id == dialogTid, "Modal wait must be tied to the heartbeat thread");
  Require(modal.window_evidence, "Helper dialog capture must provide window evidence");
  Require(modal.dialog_title == kDialogTitle, "Dialog title must survive capture and analysis");
  Require(modal.stack_evidence, "Main-thread stack must match the MessageBox wait chain");
  Require(modal.wait_api.find(L"user32.dll!MessageBox") == 0, "Wait API must be a user32 MessageBox entry point");
  Require(
    _wcsicmp(modal.caller_module_filename.c_str(), L"skydiag_modal_dialog_hang_e2e_tests.exe") == 0,
    "Dialog caller must be the module that called MessageBoxW");
  Require(modal.caller_kind == "plugin", "A non-system, non-runtime caller is classified as plugin");

  Require(result.freeze_analysis.state_id == "modal_dialog_wait", "Freeze state must be modal_dialog_wait");
  Require(
    result.freeze_analysis.confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kHigh,
    "Window and stack evidence together justify High state confidence");
  for (const auto& candidate : result.actionable_candidates) {
    for (const auto& family : candidate.supporting_families) {
      Require(
        family == "modal_dialog_owner" || family == "history_repeat",
        "Modal waits must not produce stack/frame/resource candidates");
    }
  }
  Require(
    result.summary_sentence.find(L"modal dialog") != std::wstring::npos,
    "Summary must explain the modal dialog wait");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestModalDialogHangIsClassifiedAndAttributedToDialogCaller();
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
