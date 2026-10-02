#include <cassert>
#include <string>
#include <vector>

#include "ModalDialogWait.h"
#include "WctTypes.h"

using skydiag::dump_tool::MatchModalDialogWaitStack;
using skydiag::dump_tool::ModalDialogWaitInput;
using skydiag::dump_tool::ModalStackFrame;
using skydiag::dump_tool::ResolveModalDialogWait;
using skydiag::dump_tool::internal::TryParseWctFreezeSummary;
using skydiag::dump_tool::internal::WctFreezeSummary;
using skydiag::dump_tool::internal::WctModalDialog;

namespace {

ModalStackFrame System(std::wstring module, std::wstring symbol = L"", std::uint64_t displacement = 0x40)
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = std::move(module);
  frame.symbol = std::move(symbol);
  frame.displacement = displacement;
  frame.is_system = true;
  return frame;
}

ModalStackFrame Plugin(std::wstring module, std::wstring modName = L"")
{
  ModalStackFrame frame{};
  frame.has_module = true;
  frame.module_filename = std::move(module);
  frame.inferred_mod_name = std::move(modName);
  return frame;
}

ModalStackFrame Skse()
{
  auto frame = Plugin(L"skse64_1_5_97.dll");
  frame.is_skse_runtime = true;
  frame.is_hook_framework = true;
  return frame;
}

ModalStackFrame GameExe()
{
  auto frame = Plugin(L"SkyrimSE.exe");
  frame.is_game_exe = true;
  return frame;
}

// win32u!NtUserWaitMessage <- user32 modal loop <- MessageBoxW <- caller ...
std::vector<ModalStackFrame> MessageBoxChain()
{
  return {
    System(L"win32u.dll", L"NtUserWaitMessage", 0x14),
    System(L"user32.dll", L"DialogBoxIndirectParamAorW", 0x3c0),
    System(L"user32.dll", L"", 0),  // unresolved internal frame
    System(L"user32.dll", L"SoftModalMessageBox", 0x6a2),
    System(L"user32.dll", L"MessageBoxTimeoutW", 0x1a8),
    System(L"user32.dll", L"MessageBoxW", 0x4e),
  };
}

void TestMessageBoxChainAttributesDirectPluginCaller()
{
  auto frames = MessageBoxChain();
  frames.push_back(Plugin(L"BrokenPlugin.dll", L"Broken Plugin"));
  frames.push_back(Plugin(L"ColdBreathNG.dll"));
  frames.push_back(GameExe());

  const auto match = MatchModalDialogWaitStack(frames);
  assert(match.matched);
  assert(match.wait_api == L"user32.dll!MessageBoxW");
  assert(match.api_frame_index == 5u);
  assert(match.has_caller);
  assert(frames[match.caller_frame_index].module_filename == L"BrokenPlugin.dll");
}

void TestSkseRuntimeCallerIsNotPromotedToDeeperPlugin()
{
  auto frames = MessageBoxChain();
  frames.push_back(Skse());
  frames.push_back(Plugin(L"ColdBreathNG.dll"));

  const auto match = MatchModalDialogWaitStack(frames);
  assert(match.matched);
  assert(match.has_caller);
  const auto& caller = frames[match.caller_frame_index];
  assert(caller.module_filename == L"skse64_1_5_97.dll");
  assert(skydiag::dump_tool::ClassifyModalDialogCaller(caller) == "skse_runtime");
}

void TestGameExeCallerStopsTheWalk()
{
  auto frames = MessageBoxChain();
  frames.push_back(GameExe());
  frames.push_back(Plugin(L"ColdBreathNG.dll"));

  const auto match = MatchModalDialogWaitStack(frames);
  assert(match.matched && match.has_caller);
  assert(skydiag::dump_tool::ClassifyModalDialogCaller(frames[match.caller_frame_index]) == "game_exe");
}

void TestCrtAbortDialogSkipsSystemCallerToPlugin()
{
  auto frames = MessageBoxChain();
  frames.push_back(System(L"ucrtbase.dll", L"abort", 0x4e));
  frames.push_back(Plugin(L"AssertingPlugin.dll"));

  const auto match = MatchModalDialogWaitStack(frames);
  assert(match.matched && match.has_caller);
  assert(frames[match.caller_frame_index].module_filename == L"AssertingPlugin.dll");
}

void TestPluginCodeAboveModalApiIsNotAModalWait()
{
  // A plugin window procedure dispatched from the modal loop is executing on
  // top of the dialog; that is not a pure modal wait.
  std::vector<ModalStackFrame> frames = {
    System(L"ntdll.dll", L"NtWaitForSingleObject", 0x14),
    System(L"kernelbase.dll", L"WaitForSingleObjectEx", 0x8e),
    Plugin(L"WndProcHook.dll"),
    System(L"user32.dll", L"DispatchMessageW", 0x2e0),
    System(L"user32.dll", L"MessageBoxW", 0x4e),
    Plugin(L"BrokenPlugin.dll"),
  };
  assert(!MatchModalDialogWaitStack(frames).matched);
}

void TestLargeDisplacementAndForeignModulesDoNotMatch()
{
  std::vector<ModalStackFrame> farExport = {
    System(L"win32u.dll", L"NtUserWaitMessage", 0x14),
    System(L"user32.dll", L"MessageBoxW", 0x5000),
    Plugin(L"BrokenPlugin.dll"),
  };
  assert(!MatchModalDialogWaitStack(farExport).matched);

  std::vector<ModalStackFrame> wrongModule = {
    System(L"win32u.dll", L"NtUserWaitMessage", 0x14),
    System(L"kernelbase.dll", L"MessageBoxW", 0x10),
    Plugin(L"BrokenPlugin.dll"),
  };
  assert(!MatchModalDialogWaitStack(wrongModule).matched);

  std::vector<ModalStackFrame> taskDialog = {
    System(L"win32u.dll", L"NtUserWaitMessage", 0x14),
    System(L"COMCTL32.dll", L"TaskDialogIndirect", 0x90),
    Plugin(L"TaskPlugin.dll"),
  };
  const auto match = MatchModalDialogWaitStack(taskDialog);
  assert(match.matched && match.has_caller);
  assert(match.wait_api == L"COMCTL32.dll!TaskDialogIndirect");
}

void TestParseModalDialogsFromWctJson()
{
  const std::string json =
    R"({"threads":[],"modal_dialogs":[)"
    R"({"tid":100,"class":"#32770","title":"Fatal Error","text":"Address Library missing","has_owner":true,"owner_disabled":true},)"
    R"({"tid":0,"title":"ignored zero tid"},)"
    R"({"title":"ignored missing tid"},)"
    R"("not-an-object",)"
    R"({"tid":200,"title":7,"text":"wrong-typed title is dropped"}]})";
  const auto parsed = TryParseWctFreezeSummary(json);
  assert(parsed.has_value());
  assert(parsed->modal_dialogs_captured);
  assert(parsed->modal_dialogs.size() == 2u);
  assert(parsed->modal_dialogs[0].tid == 100u);
  assert(parsed->modal_dialogs[0].title == "Fatal Error");
  assert(parsed->modal_dialogs[0].owner_disabled);
  assert(parsed->modal_dialogs[1].tid == 200u);
  assert(parsed->modal_dialogs[1].title.empty());

  const auto legacy = TryParseWctFreezeSummary(R"({"threads":[]})");
  assert(legacy.has_value());
  assert(!legacy->modal_dialogs_captured);
  assert(legacy->modal_dialogs.empty());
}

WctFreezeSummary WctWithDialog(std::uint32_t tid, std::string title, std::string text)
{
  WctFreezeSummary wct{};
  wct.has = true;
  wct.modal_dialogs_captured = true;
  WctModalDialog dialog{};
  dialog.tid = tid;
  dialog.title = std::move(title);
  dialog.text = std::move(text);
  dialog.has_owner = true;
  dialog.owner_disabled = true;
  wct.modal_dialogs.push_back(std::move(dialog));
  return wct;
}

void TestResolveWindowAndStackEvidence()
{
  const auto wct = WctWithDialog(100, "SKSE", "Plugin failed to load");
  auto frames = MessageBoxChain();
  frames.push_back(Plugin(L"BrokenPlugin.dll", L"Broken Plugin"));

  ModalDialogWaitInput input{};
  input.main_thread_id = 100;
  input.wct = &wct;
  input.main_thread_frames = &frames;
  const auto info = ResolveModalDialogWait(input);
  assert(info.detected);
  assert(info.window_evidence);
  assert(info.stack_evidence);
  assert(info.main_thread_id == 100u);
  assert(info.dialog_title == L"SKSE");
  assert(info.wait_api == L"user32.dll!MessageBoxW");
  assert(info.caller_module_filename == L"BrokenPlugin.dll");
  assert(info.caller_inferred_mod_name == L"Broken Plugin");
  assert(info.caller_kind == "plugin");
  assert(info.other_thread_dialog_count == 0u);
}

void TestResolveWindowOnlyAndOtherThreadDialogs()
{
  const auto mainDialog = WctWithDialog(100, "Error", "");
  ModalDialogWaitInput windowOnly{};
  windowOnly.main_thread_id = 100;
  windowOnly.wct = &mainDialog;
  const auto info = ResolveModalDialogWait(windowOnly);
  assert(info.detected);
  assert(info.window_evidence);
  assert(!info.stack_evidence);
  assert(info.caller_kind == "none");

  // A dialog pumped by a worker thread does not stop the main-thread heartbeat.
  const auto workerDialog = WctWithDialog(200, "Worker", "");
  ModalDialogWaitInput workerOnly{};
  workerOnly.main_thread_id = 100;
  workerOnly.wct = &workerDialog;
  const auto worker = ResolveModalDialogWait(workerOnly);
  assert(!worker.detected);
  assert(worker.other_thread_dialog_count == 1u);

  ModalDialogWaitInput noMainThread{};
  noMainThread.wct = &mainDialog;
  assert(!ResolveModalDialogWait(noMainThread).detected);
}

void TestResolveStackOnlyWithoutHelperDialogCapture()
{
  auto frames = MessageBoxChain();
  frames.push_back(Skse());
  ModalDialogWaitInput input{};
  input.main_thread_id = 100;
  input.main_thread_frames = &frames;
  const auto info = ResolveModalDialogWait(input);
  assert(info.detected);
  assert(!info.window_evidence);
  assert(info.stack_evidence);
  assert(info.caller_kind == "skse_runtime");
}

void TestDialogTextRedactsUserProfileAndFlattensLines()
{
  const auto wct = WctWithDialog(
    100,
    "Error",
    "Failed to read C:\\Users\\SomeOne\\Documents\\My Games\\x.ini\r\nSee /users/other/log.txt");
  ModalDialogWaitInput input{};
  input.main_thread_id = 100;
  input.wct = &wct;
  const auto info = ResolveModalDialogWait(input);
  assert(info.detected);
  assert(info.dialog_text ==
    L"Failed to read C:\\Users\\<user>\\Documents\\My Games\\x.ini / See /users/<user>/log.txt");
}

}  // namespace

int main()
{
  TestMessageBoxChainAttributesDirectPluginCaller();
  TestSkseRuntimeCallerIsNotPromotedToDeeperPlugin();
  TestGameExeCallerStopsTheWalk();
  TestCrtAbortDialogSkipsSystemCallerToPlugin();
  TestPluginCodeAboveModalApiIsNotAModalWait();
  TestLargeDisplacementAndForeignModulesDoNotMatch();
  TestParseModalDialogsFromWctJson();
  TestResolveWindowAndStackEvidence();
  TestResolveWindowOnlyAndOtherThreadDialogs();
  TestResolveStackOnlyWithoutHelperDialogCapture();
  TestDialogTextRedactsUserProfileAndFlattensLines();
  return 0;
}
