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
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "CrashHistory.h"
#include "DumpObjects.h"
#include "HangCaptureInternal.h"
#include "HangObjectMemory.h"
#include "HelperLog.h"
#include "HelperRuntimeTestUtils.h"
#include "MinidumpUtil.h"
#include "OutputWriterPipeline.h"
#include "RetentionWorker.h"
#include "SkyrimDiagSaveLoad.h"
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

// A reference's extra data, AE 1.6.629+ layout (shared/SkyrimDiagExtraData.h).
class BSExtraData
{
public:
  virtual ~BSExtraData() = default;
  BSExtraData* next = nullptr;  // 0x08
};

class ExtraCount : public BSExtraData
{
public:
  ~ExtraCount() override = default;
};

class ExtraLeveledCreature : public BSExtraData
{
public:
  ~ExtraLeveledCreature() override = default;
  TESBoundObject* originalBase = nullptr;  // 0x10
  TESBoundObject* templateBase = nullptr;  // 0x18
};

class BaseExtraList
{
public:
  virtual ~BaseExtraList() = default;
  BSExtraData* data = nullptr;  // 0x08
  void* presence = nullptr;     // 0x10
};

class TESObjectREFR : public TESForm
{
public:
  ~TESObjectREFR() override = default;
  char unk20[0x20]{};
  TESBoundObject* baseObject = nullptr;  // 0x40, OBJ_REFR::objectReference
  char unk48[0x28]{};
  BaseExtraList extraList;               // 0x70, ExtraDataList::_extraData
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

// An AI package and its data, as shared/SkyrimDiagPackage.h reads them.
class TESPackageData
{
public:
  virtual ~TESPackageData() = default;
};

class IPackageData
{
public:
  virtual ~IPackageData() = default;
};

class BGSPackageDataBool : public IPackageData
{
public:
  ~BGSPackageDataBool() override = default;
};

class BGSPackageDataLocation : public IPackageData
{
public:
  ~BGSPackageDataLocation() override = default;
};

class TESPackage;

class TESCustomPackageData : public TESPackageData
{
public:
  ~TESCustomPackageData() override = default;
  IPackageData** data = nullptr;          // 0x08, BGSPackageDataList::data
  std::int8_t* uids = nullptr;            // 0x10
  std::uint16_t dataSize = 0;             // 0x18
  void* procedureTree = nullptr;          // 0x20
  void* nameMap = nullptr;                // 0x28
  TESPackage* templateParent = nullptr;   // 0x30
};

class TESPackage : public TESForm
{
public:
  ~TESPackage() override = default;
  std::uint32_t packFlags = 0;            // 0x20, PACKAGE_DATA
  std::uint8_t packType = 0;              // 0x24
  std::uint8_t pad25[0x0B]{};
  TESPackageData* data = nullptr;         // 0x30
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

  // A package the game made at run time (no plugin) from Skyrim.esm's
  // package, with a data entry that is no object.
  auto templatePackage = std::make_unique<TESPackage>();
  templatePackage->sourceFiles = &npcFiles;
  templatePackage->formID = 0x00037CC0u;
  templatePackage->formType = 0x4F;
  auto boolData = std::make_unique<BGSPackageDataBool>();
  auto locationData = std::make_unique<BGSPackageDataLocation>();
  IPackageData* entries[] = { boolData.get(), locationData.get(), reinterpret_cast<IPackageData*>(std::uintptr_t{ 2 }) };
  auto customData = std::make_unique<TESCustomPackageData>();
  customData->data = entries;
  customData->dataSize = 3;
  customData->templateParent = templatePackage.get();
  auto runtimePackage = std::make_unique<TESPackage>();
  runtimePackage->formID = 0xFF0036C8u;
  runtimePackage->formType = 0x4F;
  runtimePackage->packType = 18;
  runtimePackage->data = customData.get();
  {
    const auto* pkg = reinterpret_cast<const char*>(runtimePackage.get());
    const auto* custom = reinterpret_cast<const char*>(customData.get());
    Require(reinterpret_cast<const char*>(&runtimePackage->packType) - pkg == 0x24 &&
              reinterpret_cast<const char*>(&runtimePackage->data) - pkg == 0x30 &&
              reinterpret_cast<const char*>(&customData->data) - custom == 0x08 &&
              reinterpret_cast<const char*>(&customData->dataSize) - custom == 0x18 &&
              reinterpret_cast<const char*>(&customData->templateParent) - custom == 0x30,
      "The package layout the reader assumes");
  }

  // An actor from a leveled list: a temporary base NPC the game made, and the
  // NPC it was rolled from in its extra data, after another extra.
  auto temporaryNpc = std::make_unique<TESNPC>();
  temporaryNpc->formID = 0xFF001234u;
  temporaryNpc->formType = 0x2B;
  temporaryNpc->fullName = kNpcName;
  auto count = std::make_unique<ExtraCount>();
  auto leveled = std::make_unique<ExtraLeveledCreature>();
  leveled->originalBase = npc.get();
  count->next = leveled.get();
  auto spawned = std::make_unique<Character>();
  spawned->formID = 0xFF00ABCDu;
  spawned->formType = 0x3E;
  spawned->baseObject = temporaryNpc.get();
  spawned->extraList.data = count.get();
  Require(reinterpret_cast<const char*>(&spawned->extraList) - reinterpret_cast<const char*>(spawned.get()) == 0x70 &&
            reinterpret_cast<const char*>(&leveled->originalBase) - reinterpret_cast<const char*>(leveled.get()) == 0x10,
    "The extra data layout the reader assumes");

  ParkedThread faulting;
  CONTEXT ctx = faulting.Context();
  ctx.Rcx = reinterpret_cast<DWORD64>(character.get());
  ctx.R12 = reinterpret_cast<DWORD64>(spawned.get());
  ctx.Rsi = reinterpret_cast<DWORD64>(node.get());
  ctx.Rdx = reinterpret_cast<DWORD64>(notAnObject.get());
  ctx.R13 = reinterpret_cast<DWORD64>(runtimePackage.get());

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

  // The leveled actor: its temporary base, and the NPC it was rolled from.
  const skydiag::dump_tool::DumpObject* spawnedObject = nullptr;
  for (const auto& object : result.dump_objects) {
    if (object.location == L"R12") {
      spawnedObject = &object;
    }
  }
  Require(spawnedObject != nullptr && spawnedObject->has_base_object && spawnedObject->base_form_id == 0xFF001234u &&
            spawnedObject->base_source_files.empty(),
    "The leveled actor's temporary base has no plugin");
  Require(spawnedObject->has_leveled_original && spawnedObject->leveled_original_type_name == L"TESNPC" &&
            spawnedObject->leveled_original_form_id == 0x0001D8F6u &&
            spawnedObject->leveled_original_files.size() == 1u && spawnedObject->leveled_original_files[0] == L"Skyrim.esm",
    "The NPC it was rolled from, and its plugin");
  Require(!form->has_leveled_original, "A placed actor's own base is not followed");
  Require(skydiag::dump_tool::DescribeDumpObject(*spawnedObject) ==
            L"R12: Character \"Blood Dragon\" [0xFF00ABCD] (base TESNPC [0xFF001234], leveled from TESNPC [0x0001D8F6] "
            L"Skyrim.esm)",
    "The report line");
  Require(skydiag::dump_tool::DescribeDumpObjectTarget(*spawnedObject) ==
            L"Character \"Blood Dragon\" (FormID 0xFF00ABCD, base TESNPC 0xFF001234 from TESNPC 0x0001D8F6 Skyrim.esm)",
    "The freeze clue's wording");

  // The package: what it was made from and of, one pointer past what the
  // registers reach.
  const skydiag::dump_tool::DumpObject* package = nullptr;
  for (const auto& object : result.dump_objects) {
    if (object.location == L"R13") {
      package = &object;
    }
  }
  Require(package != nullptr && package->type_name == L"TESPackage" && package->form_id == 0xFF0036C8u,
    "The package in R13 is read back");
  Require(package->is_package && package->package_type == 18u && package->package_has_template &&
            package->package_template_form_id == 0x00037CC0u && package->package_template_files.size() == 1u &&
            package->package_template_files[0] == L"Skyrim.esm",
    "The package it was made from, and its plugin");
  Require(package->package_has_data && package->package_data_count == 3u &&
            (package->package_data_types == std::vector<std::wstring>{ L"Bool", L"Location", L"?" }),
    "Its data entries, the one that is no object marked");
  Require(skydiag::dump_tool::DescribeDumpObject(*package) ==
            L"R13: TESPackage [0xFF0036C8] {type 18, made from [0x00037CC0] Skyrim.esm, data 3: Bool, Location, ?}",
    "The report line");

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
  bool hasPackageJson = false;
  for (const auto& object : summary["dump_objects"]) {
    if (object.value("location", std::string{}) == "R13") {
      hasPackageJson = object["package"]["type"] == 18 && object["package"]["template"]["form_id"] == 0x00037CC0u &&
        object["package"]["template"]["source_files"][0] == "Skyrim.esm" && object["package"]["data_count"] == 3 &&
        object["package"]["data_types"].size() == 3u && object["package"]["data_types"][2] == "?";
    }
  }
  Require(hasPackageJson, "The summary JSON carries the package");
  bool hasLeveledJson = false;
  for (const auto& object : summary["dump_objects"]) {
    if (object.value("location", std::string{}) == "R12") {
      hasLeveledJson = object["base_object"]["leveled_from"]["type"] == "TESNPC" &&
        object["base_object"]["leveled_from"]["form_id"] == 0x0001D8F6u &&
        object["base_object"]["leveled_from"]["source_files"][0] == "Skyrim.esm";
    }
  }
  Require(hasLeveledJson, "The summary JSON carries the NPC it was rolled from");

  // ADR-0029: a crash dump has no driver events stream; the helper leaves
  // them beside it, and the analyzer reads that file.
  Require(!result.gpu_driver_log.recorded, "A crash dump carries no driver events of its own");
  const auto errorUnix = static_cast<std::uint64_t>(result.dump_capture_unix) - 10u;
  std::ofstream(outBase / L"SkyrimDiag_Crash_20261010_120000_000_GpuDriverEvents.json", std::ios::binary)
    << R"({"version":1,"since_unix":)" << (errorUnix - 100u) << R"(,"until_unix":)" << (errorUnix + 20u)
    << R"(,"total":1,"complete":true,"events":[{"provider":"nvlddmkm","id":153,"level":2,"unix":)" << errorUnix
    << R"(,"message":"Restarting TDR occurred on GPUID:100"}]})";
  skydiag::dump_tool::AnalysisResult withDriverLog{};
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, withDriverLog, &err),
    "AnalyzeDump failed on the crash dump with its driver events");
  Require(withDriverLog.gpu_driver_log.recorded && withDriverLog.gpu_driver_log.events.size() == 1u,
    "The driver events beside the dump are read");
  Require(!withDriverLog.recommendations.empty() &&
            withDriverLog.recommendations.front().find(
              L"[Driver error before crash] The graphics driver logged an error 10 s before the crash") == 0u,
    "A driver error 10 s before the crash leads the advice");

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

// ADR-0019 addendum 2026-11-15: a call through a null function pointer
// stops the thread at 0, where nothing unwinds. The crash stack starts at
// the caller the call left on the stack instead of being empty. A thread
// parked in a system call stands in for the caller: its RSP points at the
// return address into the function that made the call.
void TestNullJumpCrashStackStartsAtTheCaller()
{
  ParkedThread faulting;
  CONTEXT ctx = faulting.Context();
  ctx.Rip = 0;

  auto shared = MakeSharedLayout();
  shared->header.crash_seq = 2;
  shared->header.crash.exception_code = 0xC0000005u;
  shared->header.crash.faulting_tid = faulting.tid();
  shared->header.crash.exception_addr = 0;
  shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
  shared->header.crash.exception_record.ExceptionAddress = nullptr;
  shared->header.crash.exception_record.NumberParameters = 2;
  shared->header.crash.exception_record.ExceptionInformation[0] = 8;  // execute
  shared->header.crash.exception_record.ExceptionInformation[1] = 0;
  shared->header.crash.context = ctx;

  const auto outBase = MakeTempDir(L"skydiag_null_jump_e2e");
  const auto dumpPath = outBase / L"SkyrimDiag_Crash_20261115_001549_269.dmp";
  const HANDLE process = OpenSelfProcessHandle();
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
    std::fprintf(stderr, "null jump dump failed: %ls\n", err.c_str());
  }
  Require(written, "The null jump dump must be written");

  skydiag::dump_tool::AnalyzeOptions opt{};
  opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
  opt.output_dir = outBase.wstring();
  skydiag::dump_tool::AnalysisResult result{};
  Require(
    skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), outBase.wstring(), opt, result, &err),
    "AnalyzeDump failed on the null jump dump");
  Require(result.fault_instruction.verdict == "null_jump", "The fault reads as a null jump");

  const auto& frames = result.stackwalk_display_frames;
  std::wstring shown;
  for (const auto& frame : frames) {
    shown += L"\n  " + frame;
  }
  if (frames.size() < 3u) {
    std::fprintf(stderr, "null jump crash stack:%ls\n", shown.c_str());
  }
  Require(frames.size() >= 3u, "The crash stack is walked from the caller, not left empty");
  Require(frames[0] == L"0x0", "Frame 0 is still where the call went");
  const auto contains = [&](const wchar_t* text) {
    const std::wstring needle = skydiag::dump_tool::minidump::WideLower(text);
    return std::any_of(frames.begin(), frames.end(), [&](const std::wstring& frame) {
      return skydiag::dump_tool::minidump::WideLower(frame).find(needle) != std::wstring::npos;
    });
  };
  const bool callerIsFrame1 =
    skydiag::dump_tool::minidump::WideLower(frames[1]).find(L"kernelbase.dll") != std::wstring::npos;
  if (!callerIsFrame1) {
    std::fprintf(stderr, "null jump crash stack:%ls\n", shown.c_str());
  }
  Require(callerIsFrame1, "Frame 1 is the function that made the call");
  Require(contains(L"skydiag_crash_object_e2e_tests.exe"), "The walk goes on below the caller");
  Require(result.stackwalk_primary_tid == faulting.tid(), "The crashed thread's stack is the one shown");
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

// ADR-0021: engine-lock windows of this process's own writable data (the
// test executable stands in for the game: it is the first module).
alignas(8) std::uint32_t g_registerLock[2] = { 0u, 0u };
alignas(8) std::uint32_t g_stackLock[2] = { 0u, 0u };

void TestGameLockMemoryWindows()
{
  using skydiag::helper::internal::CollectGameLockMemory;
  using skydiag::helper::internal::GameDelayStubAddress;
  using skydiag::helper::internal::HangObjectMemoryCollector;
  const HANDLE process = OpenSelfProcessHandle();
  const std::uint64_t stub = GameDelayStubAddress(process);
  Require(stub != 0u && stub == reinterpret_cast<std::uint64_t>(
                                  GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution")),
    "The game's NtDelayExecution is ours");

  // A context as the dump writer reports it: a register on one lock, a stack
  // slot on another, plus a code address, a heap pointer and a number.
  auto heap = std::make_unique<std::uint64_t[]>(4);
  std::vector<std::uint64_t> stack(0x400 / 8, 7u);
  stack[3] = reinterpret_cast<std::uint64_t>(&g_stackLock[0]);
  stack[4] = reinterpret_cast<std::uint64_t>(heap.get());
  CONTEXT c{};
  c.Rip = stub + 0x14;
  c.Rsp = reinterpret_cast<DWORD64>(stack.data());
  c.Rbx = reinterpret_cast<DWORD64>(&g_registerLock[0]);
  c.Rsi = reinterpret_cast<DWORD64>(&TestGameLockMemoryWindows);
  DumpObjectMemoryStats stats{};
  const auto ranges = CollectGameLockMemory(process, { c }, std::chrono::steady_clock::now() + std::chrono::seconds(5), stats);
  const auto covers = [&](const void* p, std::size_t size) {
    const auto a = reinterpret_cast<std::uint64_t>(p);
    return std::any_of(ranges.begin(), ranges.end(),
      [&](const auto& r) { return a >= r.base && a + size <= r.base + r.size; });
  };
  const auto image = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
  Require(covers(g_registerLock, 8) && covers(g_stackLock, 8), "Both locks are kept");
  Require(covers(reinterpret_cast<const void*>(image), 0x1000), "The image header is kept");
  Require(!covers(heap.get(), 8) && !covers(reinterpret_cast<const void*>(&TestGameLockMemoryWindows), 1),
    "No heap, no code");
  std::printf("Lock memory: threads=%u pointers=%u ranges=%u bytes=%llu time_us=%u\n", stats.lock_threads,
    stats.lock_pointers, stats.lock_ranges, static_cast<unsigned long long>(stats.lock_bytes), stats.lock_elapsed_us);
  Require(stats.lock_threads == 1u && stats.lock_pointers == 2u && stats.lock_bytes <= 0x1000u + 4u * 0x40u &&
            !stats.lock_budget_reached,
    "Small and counted");
  Require(CollectGameLockMemory(process, {}, std::chrono::steady_clock::now() + std::chrono::seconds(5), stats).empty(),
    "No waiting thread, nothing");
  DumpObjectMemoryStats late{};
  Require(CollectGameLockMemory(process, { c }, std::chrono::steady_clock::now(), late).empty() &&
            late.lock_budget_reached,
    "No time, nothing");

  // The collector keeps only game threads sitting in the delay stub.
  const DWORD tid = GetCurrentThreadId();
  HangObjectMemoryCollector collector(process, {});
  collector.EnableGameLocks({ tid }, stub);
  collector.OnThreadContext(tid + 4u, c);  // not a game thread
  CONTEXT running = c;
  running.Rip = reinterpret_cast<DWORD64>(&TestGameLockMemoryWindows);
  collector.OnThreadContext(tid, running);
  Require(collector.Collect().empty() && collector.Stats().lock_enabled && collector.Stats().lock_threads == 0u,
    "A running thread is not waiting on a lock");
  HangObjectMemoryCollector waiting(process, {});
  waiting.EnableGameLocks({ tid }, stub);
  waiting.OnThreadContext(tid, c);
  waiting.OnThreadContext(tid, c);  // ThreadExCallback after ThreadCallback: once
  const auto kept = waiting.Collect();
  Require(waiting.Stats().lock_threads == 1u && !kept.empty() && waiting.Stats().bytes == 0u,
    "The waiting thread's windows, apart from the object bytes");
  HangObjectMemoryCollector disabled(process, {});
  disabled.EnableGameLocks({ tid }, 0u);
  disabled.OnThreadContext(tid, c);
  Require(disabled.Collect().empty() && !disabled.Stats().lock_enabled, "No stub, no windows");
  CloseHandle(process);
}

// ADR-0025, from the dumps: two crashes while loading one save, in two places.
// The first analysis keeps the save in crash_history.json, and the second one
// reads it back and leads with the save.
__declspec(noinline) int OtherCrashSite(int value)
{
  return value * 3 + 1;
}

void TestRepeatedSaveLoadCrashFromTheDumps()
{
  constexpr std::string_view kSaveName = "Save76_FC70D793_0_646F726F_Tamriel_000843_20261101071101_14_1";
  const auto layoutLoading = [&](DWORD mainTid, std::uint64_t sessionStartQpc) {
    auto shared = MakeSharedLayout();
    shared->header.start_qpc = sessionStartQpc;
    const auto push = [&](std::uint32_t index, std::uint64_t qpc, skydiag::EventType type, skydiag::EventPayload payload) {
      auto& e = shared->events[index];
      e.tid = mainTid;
      e.qpc = qpc;
      e.type = static_cast<std::uint16_t>(type);
      e.size = sizeof(payload);
      e.payload = payload;
      e.seq = index * 2u;  // committed
    };
    push(0, 2, skydiag::EventType::kSessionStart, {});
    // What the plugin writes for SKSE's PreLoadGame.
    const auto words = skydiag::MakeSaveLoadStartPayload(kSaveName);
    skydiag::EventPayload save{};
    save.a = words.a;
    save.b = words.b;
    save.c = words.c;
    save.d = words.d;
    push(1, 100'000'000ull, skydiag::EventType::kSaveLoadStart, save);  // 10 s
    skydiag::EventPayload crash{};
    crash.a = 0xC0000005u;
    push(2, 290'000'000ull, skydiag::EventType::kCrash, crash);  // 29 s
    shared->header.write_index = 3;
    shared->header.state_flags = skydiag::kState_Loading;
    return shared;
  };

  const auto outBase = MakeTempDir(L"skydiag_save_load_e2e");
  const auto attemptBase = MakeTempDir(L"skydiag_save_load_attempt_e2e");
  ParkedThread faulting;
  // The header time a dump was written with (MINIDUMP_HEADER.TimeDateStamp).
  const auto headerTime = [](const std::filesystem::path& dumpPath) {
    std::ifstream file(dumpPath, std::ios::binary);
    MINIDUMP_HEADER header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    Require(file.good(), "The dump header must be read");
    return header.TimeDateStamp;
  };
  // Writes a crash dump; headerTimeOverride sets its header time (the two real
  // crashes were 971 s apart, a test writes both within a second or two).
  // A crash dump, or with freeze an automatic freeze capture taken while the
  // save was still loading (no Crash event yet).
  const auto writeIn = [&](const std::filesystem::path& dir, const wchar_t* dumpName, DWORD64 crashAddress,
                           std::uint32_t headerTimeOverride, std::uint64_t sessionStartQpc, bool freeze) {
    CONTEXT ctx = faulting.Context();
    ctx.Rip = crashAddress;
    auto shared = layoutLoading(faulting.tid(), sessionStartQpc);
    if (freeze) {
      shared->header.write_index = 2;  // the load started; no crash
    } else {
      shared->header.crash_seq = 2;
      shared->header.crash.exception_code = 0xC0000005u;
      shared->header.crash.faulting_tid = faulting.tid();
      shared->header.crash.exception_addr = crashAddress;
      shared->header.crash.exception_record.ExceptionCode = 0xC0000005u;
      shared->header.crash.exception_record.ExceptionAddress = reinterpret_cast<PVOID>(crashAddress);
      shared->header.crash.context = ctx;
    }

    const auto dumpPath = dir / dumpName;
    const HANDLE process = OpenSelfProcessHandle();
    std::wstring err;
    const bool written = WriteDumpWithStreams(
      process,
      GetCurrentProcessId(),
      dumpPath.wstring(),
      shared.get(),
      sizeof(skydiag::SharedLayout),
      {},
      {},
      /*isCrash=*/!freeze,
      ResolveDumpProfile(DumpMode::kDefault, freeze ? CaptureKind::Hang : CaptureKind::Crash),
      /*isProcessSnapshot=*/false,
      &err);
    CloseHandle(process);
    Require(written, "The dump must be written");
    if (headerTimeOverride != 0u) {
      std::fstream file(dumpPath, std::ios::in | std::ios::out | std::ios::binary);
      MINIDUMP_HEADER header{};
      file.read(reinterpret_cast<char*>(&header), sizeof(header));
      header.TimeDateStamp = headerTimeOverride;
      file.seekp(0);
      file.write(reinterpret_cast<const char*>(&header), sizeof(header));
      Require(file.good(), "The dump header time must be moved");
    }
    return dumpPath;
  };
  // Each crash ends its game, so the next load of the save is a new session.
  std::uint64_t nextSession = 1u;
  const auto write = [&](const wchar_t* dumpName, DWORD64 crashAddress, std::uint32_t headerTimeOverride) {
    return writeIn(outBase, dumpName, crashAddress, headerTimeOverride, nextSession++, /*freeze=*/false);
  };
  const auto analyzeIn = [&](const std::filesystem::path& dir, const std::filesystem::path& dumpPath) {
    skydiag::dump_tool::AnalyzeOptions opt{};
    opt.language = skydiag::dump_tool::i18n::Language::kEnglish;
    opt.output_dir = dir.wstring();
    skydiag::dump_tool::AnalysisResult result{};
    std::wstring err;
    Require(skydiag::dump_tool::AnalyzeDump(dumpPath.wstring(), dir.wstring(), opt, result, &err), "AnalyzeDump failed");
    return result;
  };
  const auto analyze = [&](const std::filesystem::path& dumpPath) { return analyzeIn(outBase, dumpPath); };

  const auto firstDump = write(L"SkyrimDiag_Crash_20261101_161808_940.dmp", faulting.Context().Rip, 1793517488u);
  const auto secondDump = write(
    L"SkyrimDiag_Crash_20261101_163419_163.dmp", reinterpret_cast<DWORD64>(&OtherCrashSite), headerTime(firstDump) + 971u);

  // In reverse order: the later crash first sees nothing earlier, and the
  // earlier one never counts the later one.
  const auto secondAlone = analyze(secondDump);
  Require(secondAlone.save_load.seen && secondAlone.save_load.in_progress &&
            secondAlone.save_load.name == L"Save76_FC70D793_0_646F7" &&
            secondAlone.save_load.seconds_loading > 18.9 && secondAlone.save_load.seconds_loading < 19.1,
    "The save being loaded is read from the dump's blackbox");
  Require(secondAlone.save_load.earlier_crashes == 0u && secondAlone.quick_answer.case_id != "damaged_save",
    "One failed load names no damaged save");
  // What the viewer compares (SaveLoadHistoryFingerprint.cs): the history the
  // later crash's judgement read, without its own rows.
  const auto historyFingerprint = [&](const skydiag::dump_tool::AnalysisResult& r) {
    skydiag::dump_tool::CrashHistory h;
    Require(h.LoadFromFile(outBase / L"crash_history.json"), "crash_history.json is read");
    return h.SaveLoadHistoryFingerprint(r.dump_identity.sha256);
  };
  Require(!secondAlone.save_load.history_fingerprint.empty() &&
            secondAlone.save_load.history_fingerprint == historyFingerprint(secondAlone),
    "Analyzed alone, the later crash's judgement matches the history");
  const auto first = analyze(firstDump);
  Require(first.dump_capture_unix + 971u == secondAlone.dump_capture_unix, "The dump header times are read");
  Require(historyFingerprint(secondAlone) != secondAlone.save_load.history_fingerprint,
    "Once the earlier crash is in the history, the later crash's stored judgement is stale");
  Require(first.save_load.earlier_crashes == 0u && first.quick_answer.case_id != "damaged_save",
    "The earlier crash does not count the later one, though it was analyzed after it");

  const auto second = analyze(secondDump);
  Require(second.save_load.history_fingerprint == historyFingerprint(second),
    "Analyzed again, its judgement matches the history it read");
  Require(!first.crash_bucket_key.empty() && second.crash_bucket_key != first.crash_bucket_key,
    "The two crashes are in different places");
  Require(second.save_load.earlier_crashes == 1u && second.save_load.crash_places == 2u &&
            second.save_load.other_save_crashes == 0u,
    "Re-analyzed, the later crash reads the earlier one's save from crash_history.json");
  Require(second.quick_answer.case_id == "damaged_save" && !second.recommendations.empty() &&
            second.recommendations[0].rfind(L"[Damaged save] ", 0) == 0,
    "The later crash leads with the save");

  // A renamed copy of the earlier dump (same bytes, a new file time) is the
  // same incident.
  const auto copy = outBase / L"copy_of_the_first_crash.dmp";
  std::filesystem::copy_file(firstDump, copy);
  std::filesystem::last_write_time(copy, std::filesystem::last_write_time(firstDump) + std::chrono::hours(1));
  (void)analyze(copy);
  const auto again = analyze(secondDump);
  Require(again.save_load.earlier_crashes == 1u && again.save_load.crash_places == 2u,
    "A copy of a dump does not count twice");

  // Start with the on-disk row an older analyzer would have left for the
  // first dump. Its bucket cannot prove a second failure place.
  const auto historyPath = outBase / L"crash_history.json";
  auto history = nlohmann::json::parse(std::ifstream(historyPath));
  auto& entries = history.at("entries");
  auto old = std::find_if(entries.begin(), entries.end(), [&](const auto& row) {
    return row.at("dump_file") == firstDump.filename().string();
  });
  Require(old != entries.end(), "The first incident is in the history");
  auto legacy = *old;
  legacy.erase("loading_attempt_key");  // older writers had no load attempts
  for (const char* placeholder : {"0xc0000005|||", "freeze|"}) {
    legacy["loading_fault_place"] = placeholder;
    entries = nlohmann::json::array({legacy});
    std::ofstream(historyPath) << history;
    const auto unknown = analyze(secondDump);
    Require(unknown.save_load.earlier_crashes == 1u && unknown.save_load.crash_places == 1u &&
              !unknown.save_load.all_places_known && unknown.quick_answer.case_id != "damaged_save",
      "A stored placeholder is not evidence of another failure place");
  }
  legacy.erase("loading_fault_place");
  legacy.erase("capture_unix");
  entries = nlohmann::json::array({legacy});
  std::ofstream(historyPath) << history;
  const auto mixed = analyze(secondDump);
  Require(mixed.save_load.earlier_crashes == 1u && mixed.save_load.crash_places == 1u &&
            !mixed.save_load.all_places_known && mixed.quick_answer.case_id != "damaged_save",
    "A legacy row adds one failed load and no evidence of another place");
  (void)analyze(copy);
  const auto restored = analyze(secondDump);
  Require(restored.save_load.earlier_crashes == 1u && restored.save_load.all_places_known &&
            restored.quick_answer.case_id == "damaged_save",
    "Reanalyzing the earlier bytes supplies their location without adding an incident");

  // Delayed analysis of the legacy row must not hide an intervening failed
  // load of a different save once a copy supplies the real capture time.
  history = nlohmann::json::parse(std::ifstream(historyPath));
  auto& restoredRows = history.at("entries");
  for (auto& row : restoredRows) {
    if (row.at("dump_file") == firstDump.filename().string()) {
      row["timestamp_utc"] = "2026-11-05T17:00:00Z";
    }
  }
  auto other = legacy;
  other["dump_file"] = "another-save.dmp";
  other["dump_identity_key"] = "other.meta";
  other["loading_save_key"] = "another-save";
  other["capture_unix"] = first.dump_capture_unix + 60u;
  restoredRows.push_back(other);
  std::ofstream(historyPath) << history;
  const auto differentSave = analyze(secondDump);
  Require(differentSave.save_load.earlier_crashes == 1u && differentSave.save_load.crash_places == 2u &&
            differentSave.save_load.other_save_crashes == 1u && differentSave.quick_answer.case_id != "damaged_save",
    "A different save failed between the two captures, even when the first was analyzed much later");

  // One load, two captures: the helper took a freeze dump 7 s into the load,
  // then the same load crashed at 19 s. Same session, same SaveLoadStart: one
  // failed load, whatever the two dumps' places are.
  constexpr std::uint32_t kLoadAt = 1793517000u;
  const auto freezeDump = writeIn(attemptBase, L"SkyrimDiag_Hang_20261101_161000_000.dmp", faulting.Context().Rip,
    kLoadAt, 7u, /*freeze=*/true);
  const auto sameLoadCrash = writeIn(attemptBase, L"SkyrimDiag_Crash_20261101_161012_000.dmp",
    reinterpret_cast<DWORD64>(&OtherCrashSite), kLoadAt + 12u, 7u, /*freeze=*/false);
  const auto frozen = analyzeIn(attemptBase, freezeDump);
  Require(frozen.is_hang_like && frozen.save_load.in_progress && !frozen.save_load.attempt_key.empty(),
    "The freeze capture is a failed load with its load attempt");
  const auto crashed = analyzeIn(attemptBase, sameLoadCrash);
  Require(crashed.save_load.attempt_key == frozen.save_load.attempt_key, "Both captures are the same load attempt");
  Require(crashed.save_load.earlier_crashes == 0u && crashed.quick_answer.case_id != "damaged_save",
    "A freeze and the crash of the same load are one failed load");

  // The save loaded again in a new session and crashed where the first load
  // crashed: two loads, one place (the first load's crash, not its freeze).
  const auto reloadSamePlace = writeIn(attemptBase, L"SkyrimDiag_Crash_20261101_162000_000.dmp",
    reinterpret_cast<DWORD64>(&OtherCrashSite), kLoadAt + 600u, 8u, /*freeze=*/false);
  const auto samePlace = analyzeIn(attemptBase, reloadSamePlace);
  Require(samePlace.save_load.attempt_key != crashed.save_load.attempt_key, "A reload is a new attempt");
  Require(samePlace.save_load.earlier_crashes == 1u && samePlace.save_load.crash_places == 1u &&
            samePlace.quick_answer.case_id != "damaged_save",
    "One earlier load whose crash was in the same place");

  // Loaded again and crashed somewhere else: two loads, two places.
  const auto reloadElsewhere = writeIn(attemptBase, L"SkyrimDiag_Crash_20261101_163000_000.dmp",
    faulting.Context().Rip, kLoadAt + 1200u, 9u, /*freeze=*/false);
  const auto elsewhere = analyzeIn(attemptBase, reloadElsewhere);
  Require(elsewhere.save_load.earlier_crashes == 2u && elsewhere.save_load.crash_places == 2u &&
            elsewhere.quick_answer.case_id == "damaged_save",
    "Loads of one save that failed in different places still lead with the save");
  Require(elsewhere.save_load.other_loaded_name.empty(), "No other save is known to have loaded yet");

  // ADR-0025 (2026-11-06): the helper recorded another save loading fine
  // before this capture, and one more after it, which must not count.
  nlohmann::json loaded{ { "version", 1 }, { "loads", nlohmann::json::array() } };
  loaded["loads"].push_back({ { "save_key", "00000000000000aa" }, { "save_name", "Save75_FC70D793_0_646F7" },
    { "loaded_unix", kLoadAt + 1100u }, { "attempt_key", "99-1-5" } });
  loaded["loads"].push_back({ { "save_key", "00000000000000bb" }, { "save_name", "Later" },
    { "loaded_unix", kLoadAt + 5000u }, { "attempt_key", "99-1-9" } });
  std::ofstream(attemptBase / skydiag::kLoadedSavesFileName) << loaded;
  const auto withOther = analyzeIn(attemptBase, reloadElsewhere);
  Require(withOther.quick_answer.case_id == "damaged_save" &&
            withOther.save_load.other_loaded_name == L"Save75_FC70D793_0_646F7" &&
            withOther.save_load.other_loaded_unix == kLoadAt + 1100u && !withOther.save_load.other_loaded_this_session,
    "Another save the helper saw load before the capture");
  Require(withOther.quick_answer.why_not_certain.find(L"Save75_FC70D793_0_646F7") != std::wstring::npos,
    "The quick answer says so");

  std::filesystem::remove_all(outBase);
  std::filesystem::remove_all(attemptBase);
}

int main()
{
  try {
    TestGameLockMemoryWindows();
    TestCrashObjectsAreReadBackFromTheDump();
    TestNullJumpCrashStackStartsAtTheCaller();
    TestRepeatedSaveLoadCrashFromTheDumps();
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
