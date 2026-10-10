// ADR-0010 end to end: a crash whose registers point at a Skyrim-like form
// object on the heap. The helper's real crash dump writer must add the object
// and its RTTI to the dump, and the production analyzer must read back the
// type, the FormID and the plugin files.

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

#include "Analyzer.h"
#include "HelperRuntimeTestUtils.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagHelper/DumpWriter.h"

using skydiag::helper::CaptureKind;
using skydiag::helper::CrashObjectMemoryBytes;
using skydiag::helper::DumpMode;
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

class TESObjectREFR : public TESForm
{
public:
  ~TESObjectREFR() override = default;
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

  auto master = std::make_unique<TESFile>();
  auto plugin = std::make_unique<TESFile>();
  strcpy_s(master->fileName, "Skyrim.esm");
  strcpy_s(plugin->fileName, "AE_StellarBlade_Doro.esp");
  TESFile* files[] = { master.get(), plugin.get() };
  TESFileArray fileArray{ files, 2 };
  character->sourceFiles = &fileArray;
  character->formID = 0xFEAD081Bu;
  character->formType = 0x3E;
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
        e.details.find(L"RCX: Character [0xFEAD081B] Skyrim.esm -> AE_StellarBlade_Doro.esp") != std::wstring::npos) {
      hasEvidence = true;
    }
  }
  Require(hasEvidence, "The objects are listed as evidence");

  std::filesystem::remove_all(outBase);
}

}  // namespace

int main()
{
  try {
    TestCrashObjectsAreReadBackFromTheDump();
    std::puts("crash object e2e tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
