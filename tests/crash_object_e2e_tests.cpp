// ADR-0010 end to end: a crash whose registers point at a Skyrim-like form
// object on the heap. The helper's real crash dump writer must add the object
// and its RTTI to the dump, and the production analyzer must read back the
// type, the FormID and the plugin files.
// Addendum 2026-10-22: the same for freeze dumps, from the registers of the
// threads that matter (main thread, its wait chain, threads WCT saw running),
// within a byte and time budget and never through a guard, no-access or
// private executable page.

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "DumpObjects.h"
#include "HangCaptureInternal.h"
#include "HangObjectMemory.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "MinidumpUtil.h"
#include "OutputWriterPipeline.h"
#include "RetentionWorker.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"

using skydiag::helper::CaptureKind;
using skydiag::helper::CrashObjectMemoryBytes;
using skydiag::helper::DumpMode;
using skydiag::helper::DumpObjectMemoryStats;
using skydiag::helper::HangObjectMemoryBytes;
using skydiag::helper::ResolveDumpProfile;
using skydiag::helper::WriteDumpWithStreams;
using skydiag::tests::runtime::MakeSharedLayout;
using skydiag::tests::runtime::MakeTempDir;
using skydiag::tests::runtime::OpenSelfProcessHandle;
using skydiag::tests::runtime::ParkedThread;
using skydiag::tests::runtime::Require;

// Global scope, like Skyrim's own classes, so the RTTI names match
// (".?AVCharacter@@" and so on). Layouts as in shared/SkyrimDiagRtti.h.
struct TESFile
{
  char unk000[0x58]{};
  char fileName[0x104]{};
};

struct TESFileArray
{
  TESFile** data = nullptr;
  std::uint32_t size = 0;
};

class TESForm
{
public:
  virtual ~TESForm() = default;
  TESFileArray* sourceFiles = nullptr;  // 0x08
  std::uint32_t formFlags = 0;          // 0x10
  std::uint32_t formID = 0;             // 0x14
  std::uint16_t inGameFormFlags = 0;    // 0x18
  std::uint8_t formType = 0;            // 0x1A
};

class TESBoundObject : public TESForm
{
public:
  ~TESBoundObject() override = default;
};

class TESFullName
{
public:
  virtual ~TESFullName() = default;
  const char* fullName = nullptr;  // 0x08, a BSFixedString in the game
};

// TESFullName lands after the TESForm part, as in the game.
class TESNPC : public TESBoundObject, public TESFullName
{
public:
  ~TESNPC() override = default;
};

class TESObjectREFR : public TESForm
{
public:
  ~TESObjectREFR() override = default;
  char unk20[0x20]{};
  TESBoundObject* baseObject = nullptr;  // 0x40, OBJ_REFR::objectReference
};

class Actor : public TESObjectREFR
{
public:
  ~Actor() override = default;
};

class Character : public Actor
{
public:
  ~Character() override = default;
};

class BSFadeNode
{
public:
  virtual ~BSFadeNode() = default;
  std::uint32_t refCount = 1;
};

namespace {

void TestCrashObjectsAreReadBackFromTheDump()
{
  // The layout the decoder assumes.
  auto character = std::make_unique<Character>();
  const auto* base = reinterpret_cast<const char*>(character.get());
  Require(reinterpret_cast<const char*>(&character->formID) - base == 0x14, "TESForm::formID at 0x14");
  Require(reinterpret_cast<const char*>(&character->formType) - base == 0x1A, "TESForm::formType at 0x1A");
  Require(reinterpret_cast<const char*>(&character->baseObject) - base == 0x40, "OBJ_REFR at 0x40");

  auto master = std::make_unique<TESFile>();
  auto plugin = std::make_unique<TESFile>();
  strcpy_s(master->fileName, "Skyrim.esm");
  strcpy_s(plugin->fileName, "AE_StellarBlade_Doro.esp");
  TESFile* files[] = { master.get(), plugin.get() };
  TESFileArray fileArray{ files, 2 };
  character->sourceFiles = &fileArray;
  character->formID = 0xFEAD081Bu;
  character->formType = 0x3E;
  // The spawned actor's base NPC, defined by the master only.
  TESFile* masterOnly[] = { master.get() };
  TESFileArray npcFiles{ masterOnly, 1 };
  auto npc = std::make_unique<TESNPC>();
  npc->sourceFiles = &npcFiles;
  npc->formID = 0x0001D8F6u;
  npc->formType = 0x2B;
  static const char kNpcName[] = "Blood Dragon";
  npc->fullName = kNpcName;
  Require(reinterpret_cast<const char*>(static_cast<TESFullName*>(npc.get())) != reinterpret_cast<const char*>(npc.get()),
    "TESFullName is not at the start of TESNPC");
  character->baseObject = npc.get();
  auto node = std::make_unique<BSFadeNode>();
  auto notAnObject = std::make_unique<std::uint64_t>(42);

  ParkedThread faulting;
  CONTEXT ctx = faulting.Context();
  ctx.Rcx = reinterpret_cast<DWORD64>(character.get());
  ctx.Rsi = reinterpret_cast<DWORD64>(node.get());
  ctx.Rdx = reinterpret_cast<DWORD64>(notAnObject.get());

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.faulting_tid = faulting.tid();
  shared->header.crash.exception_addr = ctx.Rip;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
  shared->header.crash.context = ctx;

  const auto outBase = MakeTempDir(L"skydiag_crash_object_e2e");
  const auto dumpPath = outBase / L"SkyrimDiag_Crash_20261010_120000_000.dmp";
  const HANDLE process = OpenSelfProcessHandle();
  const auto bytesBefore = CrashObjectMemoryBytes();
  std::wstring err;
  const bool written = WriteDumpWithStreams(
    process,
    GetCurrentProcessId(),
    dumpPath.wstring(),
    shared.get(),
    sizeof(skydiag::SharedLayout),
    {},
    {},
    /*isCrash=*/true,
    ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Crash),
    /*isProcessSnapshot=*/false,
    &err);
  CloseHandle(process);
  if (!written) {
    std::fprintf(stderr, "crash dump failed: %ls\n", err.c_str());
  }
  Require(written, "The crash dump must be written");
  Require(CrashObjectMemoryBytes() > bytesBefore, "The dump writer must add the crash objects' memory");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the crash dump");

  const skydiag::dump_tool::DumpObject* form = nullptr;
  const skydiag::dump_tool::DumpObject* other = nullptr;
  for (const auto& object : result.dump_objects) {
    if (object.location == L"RCX") {
      form = &object;
    } else if (object.location == L"RSI") {
      other = &object;
    }
    Require(object.location != L"RDX", "A number on the heap is not an object");
  }
  Require(form != nullptr, "The object in RCX must be read back from the dump");
  Require(form->type_name == L"Character", "The RTTI type name survives the dump");
  Require(form->is_form && form->form_id == 0xFEAD081Bu && form->form_type == 0x3E, "The TESForm fields survive the dump");
  Require(form->name == L"Blood Dragon", "The base NPC's name survives the dump");
  Require(
    form->has_base_object && form->base_type_name == L"TESNPC" && form->base_form_id == 0x0001D8F6u &&
      form->base_source_files.size() == 1u && form->base_source_files[0] == L"Skyrim.esm",
    "The base object survives the dump");
  Require(
    form->source_files.size() == 2u && form->source_files[0] == L"Skyrim.esm" &&
      form->source_files[1] == L"AE_StellarBlade_Doro.esp",
    "The plugin files that define the form survive the dump");
  Require(
    _wcsicmp(form->module.c_str(), L"skydiag_crash_object_e2e_tests.exe") == 0,
    "The RTTI comes from the module that defines the class");
  Require(other != nullptr && other->type_name == L"BSFadeNode" && !other->is_form, "Other objects are typed but not forms");

  bool hasEvidence = false;
  for (const auto& e : result.evidence) {
    if (e.title.find(L"read from the dump") != std::wstring::npos &&
        e.details.find(
          L"RCX: Character \"Blood Dragon\" [0xFEAD081B] Skyrim.esm -> AE_StellarBlade_Doro.esp "
          L"(base TESNPC [0x0001D8F6] Skyrim.esm)") != std::wstring::npos) {
      hasEvidence = true;
    }
  }
  Require(hasEvidence, "The objects are listed as evidence");

  const auto summary = skydiag::dump_tool::BuildSummaryJson(result, outBase, L"stem", false);
  bool hasJson = false;
  for (const auto& object : summary["dump_objects"]) {
    if (object.value("location", std::string{}) == "RCX") {
      hasJson = object["name"] == "Blood Dragon" && object["base_object"]["type"] == "TESNPC" &&
        object["base_object"]["form_id"] == 0x0001D8F6u && object["base_object"]["source_files"].size() == 1u &&
        object["base_object"]["source_files"][0] == "Skyrim.esm";
    }
  }
  Require(hasJson, "The summary JSON carries the name and the base object");

  std::filesystem::remove_all(outBase);
}

// Keeps a parked thread suspended with some nonvolatile registers replaced, so
// a dump of this process records those values; the thread's own context comes
// back before it runs again.
class RegistersWhileSuspended
{
public:
  template <class Set>
  RegistersWhileSuspended(DWORD tid, Set set)
    : thread_(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid))
  {
    Require(thread_ != nullptr, "OpenThread failed");
    Require(SuspendThread(thread_) != static_cast<DWORD>(-1), "SuspendThread failed");
    original_.ContextFlags = CONTEXT_FULL;
    Require(GetThreadContext(thread_, &original_) != FALSE, "GetThreadContext failed");
    CONTEXT changed = original_;
    set(changed);
    changed.ContextFlags = CONTEXT_INTEGER;
    Require(SetThreadContext(thread_, &changed) != FALSE, "SetThreadContext failed");
  }
  RegistersWhileSuspended(const RegistersWhileSuspended&) = delete;
  RegistersWhileSuspended& operator=(const RegistersWhileSuspended&) = delete;
  ~RegistersWhileSuspended()
  {
    original_.ContextFlags = CONTEXT_INTEGER;
    SetThreadContext(thread_, &original_);
    ResumeThread(thread_);
    CloseHandle(thread_);
  }

  CONTEXT Current() const
  {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(thread_, &ctx);
    return ctx;
  }

private:
  HANDLE thread_ = nullptr;
  CONTEXT original_{};
};

// A page holding something that looks like a form (a real vtable and a
// FormID), then given `protect`: the object reader must never look inside.
class FakeFormPage
{
public:
  FakeFormPage(DWORD protect, std::uint64_t vtable, std::uint32_t formId)
    : page_(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
  {
    Require(page_ != nullptr, "VirtualAlloc failed");
    std::memcpy(page_, &vtable, sizeof(vtable));
    std::memcpy(page_ + 0x14, &formId, sizeof(formId));
    DWORD old = 0;
    Require(VirtualProtect(page_, 0x1000, protect, &old) != FALSE, "VirtualProtect failed");
  }
  FakeFormPage(const FakeFormPage&) = delete;
  FakeFormPage& operator=(const FakeFormPage&) = delete;
  ~FakeFormPage() { VirtualFree(page_, 0, MEM_RELEASE); }

  DWORD64 Address() const { return reinterpret_cast<DWORD64>(page_); }
  DWORD Protection() const
  {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(page_, &info, sizeof(info));
    return info.Protect;
  }

private:
  std::uint8_t* page_;
};

// A TESNPC alone in its own page, so no other object's window covers it.
class IsolatedNpc
{
public:
  IsolatedNpc(TESFileArray* files, std::uint32_t formId, const char* name)
    : page_(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
  {
    Require(page_ != nullptr, "VirtualAlloc failed");
    npc_ = new (page_) TESNPC();
    npc_->sourceFiles = files;
    npc_->formID = formId;
    npc_->formType = 0x2B;
    npc_->fullName = name;
  }
  IsolatedNpc(const IsolatedNpc&) = delete;
  IsolatedNpc& operator=(const IsolatedNpc&) = delete;
  ~IsolatedNpc()
  {
    npc_->~TESNPC();
    VirtualFree(page_, 0, MEM_RELEASE);
  }

  DWORD64 Address() const { return reinterpret_cast<DWORD64>(npc_); }

private:
  void* page_;
  TESNPC* npc_ = nullptr;
};

std::string ThreadNode(DWORD tid, std::uint32_t status)
{
  return "{\"objectType\":8,\"objectStatus\":" + std::to_string(status) + ",\"thread\":{\"processId\":" +
    std::to_string(GetCurrentProcessId()) + ",\"threadId\":" + std::to_string(tid) +
    ",\"waitTime\":100,\"contextSwitches\":7}}";
}

// The helper's WCT layout: the main thread waits on a critical section the
// owner holds, another thread is running, a fourth is waiting on its own.
std::string SyntheticWct(DWORD mainTid, DWORD ownerTid, DWORD runningTid, DWORD idleTid)
{
  const std::string threads = "[{\"tid\":" + std::to_string(mainTid) + ",\"isCycle\":false,\"nodes\":[" +
    ThreadNode(mainTid, 3) + ",{\"objectType\":1,\"objectStatus\":6,\"objectName\":\"\"}," + ThreadNode(ownerTid, 3) +
    "]},{\"tid\":" + std::to_string(ownerTid) + ",\"isCycle\":false,\"nodes\":[" + ThreadNode(ownerTid, 3) +
    "]},{\"tid\":" + std::to_string(runningTid) + ",\"isCycle\":false,\"nodes\":[" + ThreadNode(runningTid, 2) +
    "]},{\"tid\":" + std::to_string(idleTid) + ",\"isCycle\":false,\"nodes\":[" + ThreadNode(idleTid, 3) + "]}]";
  const std::string pass = ",\"capture_usable\":true,\"threads\":" + threads + "}";
  return "{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"threads\":" + threads +
    ",\"passes\":[{\"pass_index\":0" + pass + ",{\"pass_index\":1" + pass + "],\"capture_passes\":2}";
}

const skydiag::dump_tool::FreezeThreadObjects* FindThread(
  const skydiag::dump_tool::AnalysisResult& result,
  DWORD tid)
{
  for (const auto& thread : result.freeze_thread_objects) {
    if (thread.tid == tid) {
      return &thread;
    }
  }
  return nullptr;
}

bool HasRole(const skydiag::dump_tool::FreezeThreadObjects& thread, const char* role)
{
  return std::find(thread.roles.begin(), thread.roles.end(), role) != thread.roles.end();
}

// The objects ReadDumpObjects finds in `dumpPath` from a context whose RBX is `rbx`.
std::vector<skydiag::dump_tool::DumpObject> ObjectsInDumpAt(const std::filesystem::path& dumpPath, DWORD64 rbx)
{
  HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "open dump");
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  Require(mapping != nullptr, "map dump");
  void* base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  Require(base != nullptr, "view dump");
  const auto dumpSize = static_cast<std::uint64_t>(size.QuadPart);
  const auto threads = skydiag::dump_tool::minidump::LoadThreads(base, dumpSize);
  CONTEXT ctx{};
  ctx.Rbx = rbx;
  ctx.Rsp = 0;  // no stack slots
  auto objects = skydiag::dump_tool::ReadDumpObjects(base, dumpSize, threads, {}, ctx);
  UnmapViewOfFile(base);
  CloseHandle(mapping);
  CloseHandle(file);
  return objects;
}

std::unique_ptr<skydiag::SharedLayout> SharedLayoutNamingMainThread(DWORD mainTid)
{
  auto shared = MakeSharedLayout();
  auto& sessionStart = shared->events[0];
  sessionStart.tid = mainTid;
  sessionStart.qpc = 2;
  sessionStart.type = static_cast<std::uint16_t>(skydiag::EventType::kSessionStart);
  sessionStart.seq = 0;  // ring index 0 committed (index * 2)
  shared->header.write_index = 1;
  return shared;
}

void TestHangObjectsAreReadBackFromTheDump()
{
  auto master = std::make_unique<TESFile>();
  auto follower = std::make_unique<TESFile>();
  strcpy_s(master->fileName, "Skyrim.esm");
  strcpy_s(follower->fileName, "MyFollower.esp");
  TESFile* files[] = { master.get(), follower.get() };
  TESFileArray fileArray{ files, 2 };
  TESFile* masterOnly[] = { master.get() };
  TESFileArray masterFiles{ masterOnly, 1 };

  // The main thread: an actor in RBX, and pointers that only look like forms.
  auto character = std::make_unique<Character>();
  character->sourceFiles = &fileArray;
  character->formID = 0xFEAD081Bu;
  character->formType = 0x3E;
  auto npc = std::make_unique<TESNPC>();
  npc->sourceFiles = &masterFiles;
  npc->formID = 0x0001D8F6u;
  npc->formType = 0x2B;
  static const char kNpcName[] = "Blood Dragon";
  npc->fullName = kNpcName;
  character->baseObject = npc.get();
  const std::uint64_t characterVtable = *reinterpret_cast<const std::uint64_t*>(character.get());
  FakeFormPage guard(PAGE_READWRITE | PAGE_GUARD, characterVtable, 0xBAD00001u);
  FakeFormPage noAccess(PAGE_NOACCESS, characterVtable, 0xBAD00002u);
  FakeFormPage executable(PAGE_EXECUTE_READ, characterVtable, 0xBAD00003u);
  auto number = std::make_unique<std::uint64_t>(42);
  // The lock owner, a running thread, and a thread that is neither.
  auto node = std::make_unique<BSFadeNode>();
  static const char kRunningName[] = "Lydia";
  static const char kIdleName[] = "Not Selected";
  IsolatedNpc running(&fileArray, 0x000A2C94u, kRunningName);
  IsolatedNpc idle(&masterFiles, 0x00013BBFu, kIdleName);

  ParkedThread mainThread;
  ParkedThread owner;
  ParkedThread runner;
  ParkedThread bystander;
  const std::string wct = SyntheticWct(mainThread.tid(), owner.tid(), runner.tid(), bystander.tid());
  auto shared = SharedLayoutNamingMainThread(mainThread.tid());

  const auto outBase = MakeTempDir(L"skydiag_hang_object_e2e");
  const auto dumpPath = outBase / L"SkyrimDiag_Hang_20261022_120000_000.dmp";
  DumpObjectMemoryStats stats{};
  const auto bytesBefore = HangObjectMemoryBytes();
  double dumpMs = 0.0;
  {
    RegistersWhileSuspended mainRegs(mainThread.tid(), [&](CONTEXT& c) {
      c.Rbx = reinterpret_cast<DWORD64>(character.get());
      c.Rsi = guard.Address();
      c.Rdi = noAccess.Address();
      c.R12 = executable.Address();
      c.R13 = reinterpret_cast<DWORD64>(number.get());
      c.R14 = 0x10;
    });
    RegistersWhileSuspended ownerRegs(owner.tid(), [&](CONTEXT& c) { c.Rbx = reinterpret_cast<DWORD64>(node.get()); });
    RegistersWhileSuspended runnerRegs(runner.tid(), [&](CONTEXT& c) { c.Rbx = running.Address(); });
    RegistersWhileSuspended idleRegs(bystander.tid(), [&](CONTEXT& c) { c.Rbx = idle.Address(); });
    Require(mainRegs.Current().Rbx == reinterpret_cast<DWORD64>(character.get()), "The test sets the registers");

    const HANDLE process = OpenSelfProcessHandle();
    std::wstring err;
    const auto start = std::chrono::steady_clock::now();
    const bool written = WriteDumpWithStreams(
      process,
      GetCurrentProcessId(),
      dumpPath.wstring(),
      shared.get(),
      sizeof(skydiag::SharedLayout),
      wct,
      {},
      /*isCrash=*/false,
      ResolveDumpProfile(DumpMode::kDefault, CaptureKind::Hang),
      /*isProcessSnapshot=*/false,
      &err,
      &stats);
    dumpMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CloseHandle(process);
    if (!written) {
      std::fprintf(stderr, "hang dump failed: %ls\n", err.c_str());
    }
    Require(written, "The hang dump must be written");
  }
  std::printf("hang object memory: threads=%u/%u ranges=%u bytes=%llu time_us=%u (whole dump %.1f ms, %llu bytes)\n",
    stats.threads_read, stats.threads_selected, stats.ranges, static_cast<unsigned long long>(stats.bytes),
    stats.elapsed_us, dumpMs, static_cast<unsigned long long>(std::filesystem::file_size(dumpPath)));

  // The main thread, its lock owner and the running thread; not the bystander.
  Require(stats.threads_selected == 3u && stats.threads_read == 3u, "Three threads are examined");
  Require(stats.bytes > 0u && stats.bytes <= skydiag::helper::internal::kHangObjectMemoryBudget, "Within the byte cap");
  Require(!stats.budget_reached, "A few objects stay far inside the budgets");
  Require(HangObjectMemoryBytes() - bytesBefore == stats.bytes, "The counter adds what this dump took");
  Require((guard.Protection() & PAGE_GUARD) != 0, "The guard page is never touched");

  // Only the selected threads' objects are in the dump.
  Require(!ObjectsInDumpAt(dumpPath, running.Address()).empty(), "The running thread's object is in the dump");
  Require(ObjectsInDumpAt(dumpPath, idle.Address()).empty(), "A thread nobody selected adds nothing");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the hang dump");
  Require(result.dump_objects.empty(), "A freeze has no crash objects");

  const auto* mainObjects = FindThread(result, mainThread.tid());
  Require(mainObjects && HasRole(*mainObjects, "main"), "The main thread's objects are read back");
  bool hasCharacter = false;
  for (const auto& object : mainObjects->objects) {
    if (object.location == L"RBX") {
      hasCharacter = object.type_name == L"Character" && object.form_id == 0xFEAD081Bu &&
        object.name == L"Blood Dragon" && object.source_files.size() == 2u &&
        object.source_files[1] == L"MyFollower.esp" && object.base_form_id == 0x0001D8F6u;
    }
    Require(object.form_id < 0xBAD00001u || object.form_id > 0xBAD00003u, "No fake form behind a bad page");
    Require(object.location != L"RSI" && object.location != L"RDI" && object.location != L"R12" &&
        object.location != L"R13" && object.location != L"R14",
      "Bad pointers and numbers are not objects");
  }
  Require(hasCharacter, "The actor in the main thread's RBX survives the dump");

  const auto* ownerObjects = FindThread(result, owner.tid());
  Require(ownerObjects && (HasRole(*ownerObjects, "lock_owner") || HasRole(*ownerObjects, "wait_chain")),
    "The thread the main thread waits on is read");
  Require(std::any_of(ownerObjects->objects.begin(), ownerObjects->objects.end(),
            [](const auto& o) { return o.location == L"RBX" && o.type_name == L"BSFadeNode" && !o.is_form; }),
    "The owner's object is typed");

  const auto* runnerObjects = FindThread(result, runner.tid());
  Require(runnerObjects && HasRole(*runnerObjects, "running"), "A thread WCT saw running is read");
  Require(std::any_of(runnerObjects->objects.begin(), runnerObjects->objects.end(),
            [](const auto& o) { return o.type_name == L"TESNPC" && o.name == L"Lydia" && o.form_id == 0x000A2C94u; }),
    "The running thread's form survives the dump");
  Require(FindThread(result, bystander.tid()) == nullptr, "The bystander is not reported");

  // No mod is named from an object.
  for (const auto& suspect : result.suspects) {
    Require(suspect.module_filename.find(L".esp") == std::wstring::npos, "A plugin file is never a suspect");
  }

  const auto summary = skydiag::dump_tool::BuildSummaryJson(result, outBase, L"stem", true);
  bool jsonMain = false;
  for (const auto& thread : summary["freeze_analysis"]["thread_objects"]) {
    if (thread["tid"] == mainThread.tid()) {
      for (const auto& object : thread["objects"]) {
        jsonMain = jsonMain || (object["type"] == "Character" && object["form_id"] == 0xFEAD081Bu &&
                                 object["source_files"][1] == "MyFollower.esp");
      }
      Require(thread["roles"][0] == "main", "roles in the summary");
    }
  }
  Require(jsonMain, "freeze_analysis.thread_objects carries the main thread's form");
  const auto report = skydiag::dump_tool::BuildReportText(result, summary, true);
  const auto at = report.find("  thread_objects tid=" + std::to_string(mainThread.tid()) + " roles=main");
  Require(at != std::string::npos, "The report lists the thread's objects");
  Require(report.find("RBX: Character \"Blood Dragon\" [0xFEAD081B] Skyrim.esm -> MyFollower.esp", at) !=
      std::string::npos,
    "The report names the form");
  Require(report.find(outBase.string()) == std::string::npos, "No path in the redacted report");

  std::filesystem::remove_all(outBase);
}

// Byte and time budgets of the collector itself, on a live thread.
void TestHangObjectBudgets()
{
  auto master = std::make_unique<TESFile>();
  strcpy_s(master->fileName, "Skyrim.esm");
  TESFile* masterOnly[] = { master.get() };
  TESFileArray files{ masterOnly, 1 };
  auto character = std::make_unique<Character>();
  character->sourceFiles = &files;
  character->formID = 0x00000014u;
  ParkedThread parked;
  RegistersWhileSuspended regs(parked.tid(), [&](CONTEXT& c) { c.Rbx = reinterpret_cast<DWORD64>(character.get()); });
  const CONTEXT ctx = regs.Current();
  const HANDLE process = OpenSelfProcessHandle();
  using skydiag::helper::internal::HangObjectMemoryCollector;

  HangObjectMemoryCollector unbounded(process, { parked.tid() });
  unbounded.OnThreadContext(parked.tid(), ctx);
  unbounded.OnThreadContext(parked.tid() + 4u, ctx);  // not selected
  const auto full = unbounded.Collect();
  Require(!full.empty() && unbounded.Stats().threads_read == 1u, "The object is collected");
  const std::uint64_t needed = unbounded.Stats().bytes;

  HangObjectMemoryCollector small(process, { parked.tid() }, /*budgetBytes=*/0x80);
  small.OnThreadContext(parked.tid(), ctx);
  small.Collect();
  Require(small.Stats().bytes <= 0x80u && small.Stats().bytes < needed, "The byte budget is a hard cap");

  HangObjectMemoryCollector none(process, { parked.tid() }, /*budgetBytes=*/0);
  none.OnThreadContext(parked.tid(), ctx);
  Require(none.Collect().empty() && none.Stats().budget_reached, "No budget, nothing read");

  HangObjectMemoryCollector late(process, { parked.tid() }, skydiag::helper::internal::kHangObjectMemoryBudget,
    std::chrono::milliseconds{ 0 });
  late.OnThreadContext(parked.tid(), ctx);
  Require(late.Collect().empty() && late.Stats().budget_reached && late.Stats().threads_read == 0u,
    "No time, nothing read");

  HangObjectMemoryCollector missing(process, { parked.tid() });
  Require(missing.Collect().empty() && missing.Stats().threads_read == 0u, "A thread the writer never reported");
  CloseHandle(process);
}

// The helper's real hang capture (real WCT, real dump writer) of this process,
// whose main thread holds an actor in RBX: the helper log says what it added,
// and the analyzer reads the actor back.
void TestRealHangCaptureKeepsTheMainThreadsObject()
{
  auto master = std::make_unique<TESFile>();
  auto plugin = std::make_unique<TESFile>();
  strcpy_s(master->fileName, "Skyrim.esm");
  strcpy_s(plugin->fileName, "AE_StellarBlade_Doro.esp");
  TESFile* files[] = { master.get(), plugin.get() };
  TESFileArray fileArray{ files, 2 };
  auto character = std::make_unique<Character>();
  character->sourceFiles = &fileArray;
  character->formID = 0xFEAD081Bu;
  character->formType = 0x3E;

  ParkedThread mainThread;
  auto shared = SharedLayoutNamingMainThread(mainThread.tid());
  shared->header.last_heartbeat_qpc = 1;
  const auto outBase = MakeTempDir(L"skydiag_hang_object_real");
  skydiag::helper::internal::ClearLog(outBase);
  auto proc = skydiag::tests::runtime::MakeSelfAttachedProcess(shared.get());
  skydiag::helper::HangDecision decision{};
  decision.isHang = true;
  decision.secondsSinceHeartbeat = 30.0;
  decision.thresholdSec = 10;
  std::wstring pendingViewer;
  skydiag::helper::internal::HangCaptureState state{};
  {
    RegistersWhileSuspended regs(mainThread.tid(), [&](CONTEXT& c) { c.Rbx = reinterpret_cast<DWORD64>(character.get()); });
    skydiag::helper::internal::ExecuteConfirmedHangCapture(
      skydiag::tests::runtime::MakeTestConfig(), proc, outBase, decision, 0u, &pendingViewer, &state);
  }
  skydiag::helper::internal::ShutdownRetentionWorker();
  skydiag::tests::runtime::CloseAttachedProcess(&proc);

  const auto log = skydiag::tests::runtime::ReadAllTextUtf8(outBase / L"SkyrimDiagHelper.log");
  const auto line = log.find("Object memory: threads=");
  Require(line != std::string::npos, "The helper log says what the object memory added");
  std::printf("helper log: %s\n", log.substr(line, log.find('\n', line) - line).c_str());

  const auto dumpPath = skydiag::tests::runtime::FindSingleFileWithExt(outBase, L".dmp");
  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kKorean;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  std::wstring err;
  Require(skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the real hang capture");
  const auto* mainObjects = FindThread(result, mainThread.tid());
  Require(mainObjects != nullptr, "The main thread's objects are read back");
  Require(std::any_of(mainObjects->objects.begin(), mainObjects->objects.end(),
            [](const auto& o) { return o.location == L"RBX" && o.form_id == 0xFEAD081Bu; }),
    "The actor in RBX survives the real capture");
  const auto summary = skydiag::dump_tool::BuildSummaryJson(result, outBase, L"stem", true);
  const auto report = skydiag::dump_tool::BuildReportText(result, summary, true);
  for (std::size_t at = report.find("  thread_objects "); at != std::string::npos;
       at = report.find("  thread_objects ", at + 1)) {
    std::printf("%s\n", report.substr(at, report.find('\n', report.find('\n', at) + 1) - at).c_str());
  }
  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestCrashObjectsAreReadBackFromTheDump();
    TestHangObjectsAreReadBackFromTheDump();
    TestHangObjectBudgets();
    TestRealHangCaptureKeepsTheMainThreadsObject();
    std::puts("crash object e2e tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
