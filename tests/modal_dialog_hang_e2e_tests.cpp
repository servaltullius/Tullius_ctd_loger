#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
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

// What the dialog thread's MessageBoxW did, for the failure message.
struct DialogThreadResult
{
  std::atomic<bool> returned{ false };
  std::atomic<int> result{ 0 };
  std::atomic<DWORD> error{ 0 };
};

bool WaitForDialogOwnedBy(DWORD tid, std::string* seen)
{
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::string pass;
    for (const auto& dialog : skydiag::helper::CaptureModalDialogs(GetCurrentProcessId())) {
      if (dialog.tid == tid && dialog.title == "SkyrimDiag modal hang e2e") {
        return true;
      }
      pass += " [tid " + std::to_string(dialog.tid) + " '" + dialog.title + "']";
    }
    if (seen) {
      *seen = pass.empty() ? " none" : pass;
    }
    Sleep(50);
  }
  return false;
}

BOOL CALLBACK DescribeThreadWindow(HWND hwnd, LPARAM lParam)
{
  auto* out = reinterpret_cast<std::string*>(lParam);
  wchar_t cls[64]{};
  GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
  wchar_t title[128]{};
  // Does not send WM_GETTEXT, so a stuck thread cannot block it.
  InternalGetWindowText(hwnd, title, static_cast<int>(std::size(title)));
  std::string line = " [class '";
  for (const wchar_t* p = cls; *p; ++p) {
    line += static_cast<char>(*p < 128 ? *p : '?');
  }
  line += "' title '";
  for (const wchar_t* p = title; *p; ++p) {
    line += static_cast<char>(*p < 128 ? *p : '?');
  }
  line += "' visible=" + std::to_string(IsWindowVisible(hwnd) ? 1 : 0) + "]";
  *out += line;
  return TRUE;
}

// Why no dialog was found: whether MessageBoxW already returned (and how), the
// thread's own windows, and the dialogs the probe saw in this process.
std::string DescribeMissingDialog(DWORD tid, const DialogThreadResult& box, ULONGLONG waitedMs, const std::string& seen)
{
  std::string message = "Test MessageBox never became visible (waited " + std::to_string(waitedMs) + " ms; ";
  if (box.returned.load()) {
    message += "MessageBoxW returned " + std::to_string(box.result.load()) + ", error " + std::to_string(box.error.load());
  } else {
    message += "MessageBoxW has not returned";
  }
  std::string windows;
  EnumThreadWindows(tid, DescribeThreadWindow, reinterpret_cast<LPARAM>(&windows));
  message += "; thread " + std::to_string(tid) + " windows:" + (windows.empty() ? std::string(" none") : windows);
  message += "; dialogs the probe saw last:" + seen + ")";
  return message;
}

// Captures a real hang dump while a thread that the blackbox names as the game
// main thread is parked in MessageBoxW, then runs the production analyzer on it.
void TestModalDialogHangIsClassifiedAndAttributedToDialogCaller()
{
  const auto outBase = MakeTempDir(L"skydiag_modal_dialog_hang_e2e");
  ClearLog(outBase);

  DWORD dialogTid = 0;
  DialogThreadResult box;
  HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(started != nullptr, "CreateEventW failed");
  std::thread dialogThread([&]() {
    dialogTid = GetCurrentThreadId();
    SetEvent(started);
    const int result =
      MessageBoxW(nullptr, L"An SKSE plugin reported a fatal load error.", kDialogTitle, MB_OK | MB_ICONERROR);
    box.error = GetLastError();
    box.result = result;
    box.returned = true;
  });
  WaitForSingleObject(started, 5000);
  CloseHandle(started);
  const ULONGLONG waitStart = GetTickCount64();
  std::string dialogsSeen;
  const bool dialogVisible = WaitForDialogOwnedBy(dialogTid, &dialogsSeen);
  const std::string missingDialog =
    dialogVisible ? std::string{} : DescribeMissingDialog(dialogTid, box, GetTickCount64() - waitStart, dialogsSeen);

  auto shared = MakeSharedLayout();
  shared->header.last_heartbeat_qpc = 1;
  auto& sessionStart = shared->events[0];
  sessionStart.tid = dialogTid;
  sessionStart.qpc = 2;
  sessionStart.type = static_cast<std::uint16_t>(skydiag::EventType::kSessionStart);
  sessionStart.seq = 0;  // ring index 0 committed (index * 2)
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

  Require(
    skydiag::tests::runtime::CloseDialogAndJoin(dialogThread, kDialogTitle), "Test MessageBox did not close");
  ShutdownRetentionWorker();
  CloseAttachedProcess(&proc);

  Require(dialogVisible, missingDialog);
  Require(
    state.modalDialogSeenThisEpisode,
    "A dialog at the hang capture is remembered, so a later abnormal exit keeps its WER hint");
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
  Require(modal.main_thread_id == dialogTid, "Modal wait must be tied to the main thread SessionStart names");
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
