// shared/SkyrimDiagRtti.h over a hand-built address space: MSVC x64 RTTI
// (locator, type descriptors, class hierarchy) in a fake module, and a
// Character-like object on the "heap" whose TESForm part names two plugins.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "SkyrimDiagRtti.h"

using skydiag::rtti::ReadObject;
using skydiag::rtti::UndecorateTypeName;

namespace {

class FakeMemory
{
public:
  void Put(std::uint64_t address, const void* data, std::size_t size)
  {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      bytes_[address + i] = bytes[i];
    }
  }
  template <class T>
  void PutValue(std::uint64_t address, const T& value)
  {
    Put(address, &value, sizeof(value));
  }
  void PutString(std::uint64_t address, const std::string& text) { Put(address, text.c_str(), text.size() + 1); }
  void Erase(std::uint64_t address, std::size_t size)
  {
    for (std::size_t i = 0; i < size; ++i) {
      bytes_.erase(address + i);
    }
  }

  bool operator()(std::uint64_t address, void* destination, std::size_t size) const
  {
    auto* out = static_cast<std::uint8_t*>(destination);
    for (std::size_t i = 0; i < size; ++i) {
      const auto it = bytes_.find(address + i);
      if (it == bytes_.end()) {
        return false;
      }
      out[i] = it->second;
    }
    return true;
  }

private:
  std::map<std::uint64_t, std::uint8_t> bytes_;
};

constexpr std::uint64_t kModule = 0x140000000ull;
constexpr std::uint64_t kObject = 0x2A000001000ull;
constexpr std::uint64_t kFileArray = 0x2A000002000ull;
constexpr std::uint64_t kFiles = 0x2A000003000ull;
constexpr std::uint64_t kFileA = 0x2A000004000ull;
constexpr std::uint64_t kFileB = 0x2A000005000ull;

// RVAs inside the fake module.
constexpr std::uint32_t kVtable = 0x1000;
constexpr std::uint32_t kLocator = 0x2000;
constexpr std::uint32_t kHierarchy = 0x2100;
constexpr std::uint32_t kBaseArray = 0x2200;

void PutTypeDescriptor(FakeMemory& m, std::uint32_t rva, const std::string& name)
{
  m.PutValue<std::uint64_t>(kModule + rva, 0);  // type_info vtable (unused)
  m.PutValue<std::uint64_t>(kModule + rva + 8, 0);
  m.PutString(kModule + rva + 0x10, name);
}

// Character : Actor : TESObjectREFR : TESForm, all at offset 0.
FakeMemory MakeCharacter()
{
  FakeMemory m;
  const std::vector<std::string> names = {
    ".?AVCharacter@@", ".?AVActor@@", ".?AVTESObjectREFR@@", ".?AVTESForm@@", ".?AVBaseFormComponent@@",
  };
  std::vector<std::uint32_t> descriptors;
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto td = static_cast<std::uint32_t>(0x3000 + i * 0x100);
    PutTypeDescriptor(m, td, names[i]);
    const auto bcd = static_cast<std::uint32_t>(0x4000 + i * 0x40);
    skydiag::rtti::detail::BaseClassDescriptor base{ td, 0, /*mdisp=*/0, /*pdisp=*/-1, 0, 0, kHierarchy };
    m.PutValue(kModule + bcd, base);
    descriptors.push_back(bcd);
  }
  for (std::size_t i = 0; i < descriptors.size(); ++i) {
    m.PutValue(kModule + kBaseArray + 4 * i, descriptors[i]);
  }
  skydiag::rtti::detail::ClassHierarchyDescriptor hierarchy{ 0, 0, static_cast<std::uint32_t>(names.size()), kBaseArray };
  m.PutValue(kModule + kHierarchy, hierarchy);
  skydiag::rtti::detail::CompleteObjectLocator locator{ 1, 0, 0, 0x3000, kHierarchy, kLocator };
  m.PutValue(kModule + kLocator, locator);
  m.PutValue<std::uint64_t>(kModule + kVtable - 8, kModule + kLocator);
  m.PutValue<std::uint64_t>(kModule + kVtable, kModule + 0x5000);  // first virtual function

  // The object: vtable, sourceFiles, flags, formID, inGameFlags, formType.
  m.PutValue<std::uint64_t>(kObject, kModule + kVtable);
  m.PutValue<std::uint64_t>(kObject + 0x08, kFileArray);
  m.PutValue<std::uint32_t>(kObject + 0x10, 0);
  m.PutValue<std::uint32_t>(kObject + 0x14, 0xFEAD081Bu);
  m.PutValue<std::uint16_t>(kObject + 0x18, 0);
  m.PutValue<std::uint8_t>(kObject + 0x1A, 0x3E);
  // BSStaticArray<TESFile*>: data, size.
  m.PutValue<std::uint64_t>(kFileArray, kFiles);
  m.PutValue<std::uint32_t>(kFileArray + 8, 2);
  m.PutValue<std::uint64_t>(kFiles, kFileA);
  m.PutValue<std::uint64_t>(kFiles + 8, kFileB);
  m.PutString(kFileA + 0x58, "Skyrim.esm");
  m.PutString(kFileB + 0x58, "AE_StellarBlade_Doro.esp");
  return m;
}

void TestCharacterForm()
{
  const auto m = MakeCharacter();
  const auto object = ReadObject(m, kObject);
  assert(object.has_value());
  assert(object->type_name == ".?AVCharacter@@");
  assert(object->module_base == kModule);
  assert(object->complete_object == kObject);
  assert(object->base_names.size() == 5u && object->base_names[3] == ".?AVTESForm@@");
  assert(object->is_form);
  assert(object->form_id == 0xFEAD081Bu);
  assert(object->form_type == 0x3E);
  assert((object->source_files == std::vector<std::string>{ "Skyrim.esm", "AE_StellarBlade_Doro.esp" }));
}

void TestNotAnObject()
{
  auto m = MakeCharacter();
  assert(!ReadObject(m, 0));
  assert(!ReadObject(m, kObject + 4));             // misaligned
  assert(!ReadObject(m, 0x2A000009000ull));        // unreadable
  m.PutValue<std::uint64_t>(0x2A00000A000ull, 42);  // a number, not a vtable
  assert(!ReadObject(m, 0x2A00000A000ull));

  auto badSignature = MakeCharacter();
  badSignature.PutValue<std::uint32_t>(kModule + kLocator, 0);
  assert(!ReadObject(badSignature, kObject));

  auto notRtti = MakeCharacter();
  notRtti.PutString(kModule + 0x3000 + 0x10, "Character");  // not ".?A..."
  assert(!ReadObject(notRtti, kObject));
}

void TestPartialMemory()
{
  // No class hierarchy in the dump: the type is still known, the form is not.
  auto noHierarchy = MakeCharacter();
  noHierarchy.Erase(kModule + kHierarchy, 0x10);
  const auto typed = ReadObject(noHierarchy, kObject);
  assert(typed && typed->type_name == ".?AVCharacter@@" && !typed->is_form && typed->base_names.empty());

  // A form created at runtime has no source files.
  auto runtimeForm = MakeCharacter();
  runtimeForm.PutValue<std::uint64_t>(kObject + 0x08, 0);
  const auto created = ReadObject(runtimeForm, kObject);
  assert(created && created->is_form && created->source_files.empty());

  // The second file's name is missing: keep the first.
  auto oneFile = MakeCharacter();
  oneFile.Erase(kFileB + 0x58, 32);
  const auto first = ReadObject(oneFile, kObject);
  assert(first && (first->source_files == std::vector<std::string>{ "Skyrim.esm" }));

  // A type name ending right before unreadable memory still decodes (the
  // 16-byte read fails, single bytes do not).
  auto shortName = MakeCharacter();
  shortName.Erase(kModule + 0x3000 + 0x10, 0x40);
  shortName.PutString(kModule + 0x3000 + 0x10, ".?AVHuman@@");  // 12 bytes incl. NUL, nothing after
  const auto named = ReadObject(shortName, kObject);
  assert(named && named->type_name == ".?AVHuman@@");
}

void TestNonFormClass()
{
  auto m = MakeCharacter();
  // Same object, but the hierarchy no longer contains TESForm.
  PutTypeDescriptor(m, 0x3300, ".?AVNiObject@@");
  const auto object = ReadObject(m, kObject);
  assert(object && !object->is_form && object->source_files.empty());
}

void TestUndecorate()
{
  assert(UndecorateTypeName(".?AVCharacter@@") == "Character");
  assert(UndecorateTypeName(".?AUSomething@@") == "Something");
  assert(UndecorateTypeName(".?AVPlayerCharacter@RE@@") == "RE::PlayerCharacter");
  assert(UndecorateTypeName(".?AV?$BSTEventSink@VMenuOpenCloseEvent@@@@") == "?$BSTEventSink@VMenuOpenCloseEvent@@");
  assert(UndecorateTypeName("not decorated") == "not decorated");
}

}  // namespace

int main()
{
  TestCharacterForm();
  TestNotAnObject();
  TestPartialMemory();
  TestNonFormClass();
  TestUndecorate();
  std::puts("rtti decoder tests passed");
  return 0;
}
