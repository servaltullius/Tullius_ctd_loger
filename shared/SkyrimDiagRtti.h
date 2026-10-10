#pragma once

// Reads what kind of object a pointer points at, from MSVC x64 RTTI, and for
// Skyrim forms the FormID and the plugin files that define it.
//
// Shared by the helper and the analyzer (ADR-0010). At crash time the helper
// runs it over the live game process for the crash registers and stack, and
// adds every range it read to the dump; the analyzer then runs it over the
// dump and gets the same answers. The reader is all-or-nothing:
//   bool read(std::uint64_t address, void* destination, std::size_t size)
//
// Layouts:
// - MSVC x64 RTTI: the qword before a vtable points at the
//   CompleteObjectLocator (signature 1); its type descriptor, class hierarchy
//   and base class descriptors are RVAs from the module base, which the
//   locator's own RVA gives.
// - Skyrim SE/AE TESForm: sourceFiles (TESFileContainer, a pointer to
//   BSStaticArray<TESFile*>) at 0x08, formID at 0x14, formType at 0x1A;
//   TESFile::fileName at 0x58.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::rtti {

struct ObjectInfo
{
  std::uint64_t address = 0;          // the pointer that was examined
  std::uint64_t complete_object = 0;  // address minus the locator's offset
  std::uint64_t vtable = 0;
  std::uint64_t module_base = 0;      // module that holds the RTTI
  std::string type_name;              // decorated, e.g. ".?AVCharacter@@"
  std::vector<std::string> base_names;  // decorated; the class itself comes first
  bool is_form = false;
  std::uint32_t form_id = 0;
  std::uint8_t form_type = 0;
  std::vector<std::string> source_files;  // TESFile::fileName, in load order
};

inline constexpr std::size_t kMaxTypeNameBytes = 256;
inline constexpr std::uint32_t kMaxBaseClasses = 64;
inline constexpr std::uint32_t kMaxSourceFiles = 16;
inline constexpr std::size_t kMaxFileNameBytes = 0x104;
inline constexpr std::uint64_t kMinUserAddress = 0x10000;
inline constexpr std::uint64_t kMaxUserAddress = 0x7FFFFFFF0000ull;
inline constexpr std::string_view kTesFormTypeName = ".?AVTESForm@@";

namespace detail {

inline bool PlausiblePointer(std::uint64_t p)
{
  return p >= kMinUserAddress && p < kMaxUserAddress;
}

template <class Reader, class T>
bool ReadValue(const Reader& read, std::uint64_t address, T& out)
{
  return read(address, &out, sizeof(T));
}

// Reads a NUL-terminated string in 16-byte pieces, falling back to single
// bytes when a piece crosses into memory that cannot be read.
template <class Reader>
bool ReadCString(const Reader& read, std::uint64_t address, std::size_t maxBytes, bool asciiOnly, std::string& out)
{
  out.clear();
  constexpr std::size_t kPiece = 16;
  char piece[kPiece]{};
  for (std::size_t offset = 0; offset < maxBytes;) {
    std::size_t got = kPiece;
    if (!read(address + offset, piece, kPiece)) {
      if (!read(address + offset, piece, 1)) {
        return false;
      }
      got = 1;
    }
    for (std::size_t i = 0; i < got; ++i) {
      const auto c = static_cast<unsigned char>(piece[i]);
      if (c == 0) {
        return !out.empty();
      }
      if (c < 0x20 || c == 0x7F || (asciiOnly && c > 0x7E)) {
        return false;
      }
      out.push_back(static_cast<char>(c));
    }
    offset += got;
  }
  return false;  // no terminator within maxBytes
}

#pragma pack(push, 4)
struct CompleteObjectLocator
{
  std::uint32_t signature;
  std::uint32_t offset;
  std::uint32_t cd_offset;
  std::uint32_t type_descriptor;
  std::uint32_t class_descriptor;
  std::uint32_t self;
};
struct ClassHierarchyDescriptor
{
  std::uint32_t signature;
  std::uint32_t attributes;
  std::uint32_t num_base_classes;
  std::uint32_t base_class_array;
};
struct BaseClassDescriptor
{
  std::uint32_t type_descriptor;
  std::uint32_t num_contained_bases;
  std::int32_t mdisp;
  std::int32_t pdisp;
  std::int32_t vdisp;
  std::uint32_t attributes;
  std::uint32_t class_descriptor;
};
#pragma pack(pop)
static_assert(sizeof(CompleteObjectLocator) == 0x18);
static_assert(sizeof(ClassHierarchyDescriptor) == 0x10);
static_assert(sizeof(BaseClassDescriptor) == 0x1C);

template <class Reader>
void ReadFormFields(const Reader& read, std::uint64_t form, ObjectInfo& info)
{
  std::uint32_t formId = 0;
  std::uint8_t formType = 0;
  if (!ReadValue(read, form + 0x14, formId) || !ReadValue(read, form + 0x1A, formType)) {
    return;
  }
  info.is_form = true;
  info.form_id = formId;
  info.form_type = formType;

  std::uint64_t fileArray = 0;
  if (!ReadValue(read, form + 0x08, fileArray) || !PlausiblePointer(fileArray)) {
    return;  // forms created at runtime have no source file
  }
  std::uint64_t files = 0;
  std::uint32_t count = 0;
  if (!ReadValue(read, fileArray, files) || !ReadValue(read, fileArray + 8, count) || !PlausiblePointer(files)) {
    return;
  }
  count = std::min(count, kMaxSourceFiles);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t file = 0;
    std::string name;
    if (!ReadValue(read, files + 8ull * i, file) || !PlausiblePointer(file) ||
        !ReadCString(read, file + 0x58, kMaxFileNameBytes, /*asciiOnly=*/false, name)) {
      break;
    }
    info.source_files.push_back(std::move(name));
  }
}

}  // namespace detail

// Returns nullopt unless `address` points at an object with a vtable whose
// RTTI is readable and well formed.
template <class Reader>
std::optional<ObjectInfo> ReadObject(const Reader& read, std::uint64_t address)
{
  using namespace detail;
  if (!PlausiblePointer(address) || (address & 7u) != 0) {
    return std::nullopt;
  }
  std::uint64_t vtable = 0;
  if (!ReadValue(read, address, vtable) || !PlausiblePointer(vtable) || (vtable & 7u) != 0) {
    return std::nullopt;
  }
  std::uint64_t locatorAddress = 0;
  if (!ReadValue(read, vtable - 8, locatorAddress) || !PlausiblePointer(locatorAddress) || (locatorAddress & 3u) != 0) {
    return std::nullopt;
  }
  CompleteObjectLocator locator{};
  if (!ReadValue(read, locatorAddress, locator) || locator.signature != 1 || locator.self == 0 ||
      locator.self > locatorAddress || locator.offset > address) {
    return std::nullopt;
  }
  const std::uint64_t moduleBase = locatorAddress - locator.self;
  if ((moduleBase & 0xFFFFu) != 0) {
    return std::nullopt;  // modules load at 64 KB boundaries
  }

  ObjectInfo info{};
  info.address = address;
  info.complete_object = address - locator.offset;
  info.vtable = vtable;
  info.module_base = moduleBase;
  if (!ReadCString(read, moduleBase + locator.type_descriptor + 0x10, kMaxTypeNameBytes, /*asciiOnly=*/true,
        info.type_name) ||
      info.type_name.rfind(".?A", 0) != 0) {
    return std::nullopt;
  }

  std::optional<std::uint64_t> formBase;
  ClassHierarchyDescriptor hierarchy{};
  if (ReadValue(read, moduleBase + locator.class_descriptor, hierarchy) &&
      hierarchy.num_base_classes <= kMaxBaseClasses) {
    for (std::uint32_t i = 0; i < hierarchy.num_base_classes; ++i) {
      std::uint32_t descriptorRva = 0;
      BaseClassDescriptor base{};
      std::string name;
      if (!ReadValue(read, moduleBase + hierarchy.base_class_array + 4ull * i, descriptorRva) ||
          !ReadValue(read, moduleBase + descriptorRva, base) ||
          !ReadCString(read, moduleBase + base.type_descriptor + 0x10, kMaxTypeNameBytes, /*asciiOnly=*/true, name)) {
        break;
      }
      // pdisp == -1: a non-virtual base at a fixed offset (mdisp).
      if (!formBase && name == kTesFormTypeName && base.pdisp == -1 && base.mdisp >= 0) {
        formBase = info.complete_object + static_cast<std::uint64_t>(base.mdisp);
      }
      info.base_names.push_back(std::move(name));
    }
  }
  if (formBase) {
    ReadFormFields(read, *formBase, info);
  }
  return info;
}

// ".?AVCharacter@@" -> "Character", ".?AVPlayer@RE@@" -> "RE::Player".
// Template names (".?AV?$BSTEventSink@...") keep their decorated body.
inline std::string UndecorateTypeName(std::string_view decorated)
{
  std::string_view body = decorated;
  if (body.rfind(".?AV", 0) == 0 || body.rfind(".?AU", 0) == 0) {
    body.remove_prefix(4);
  } else {
    return std::string(decorated);
  }
  if (body.size() >= 2 && body.substr(body.size() - 2) == "@@") {
    body.remove_suffix(2);
  }
  if (body.rfind("?$", 0) == 0 || body.find('?') != std::string_view::npos) {
    return std::string(body);
  }
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t at = body.find('@', start);
    parts.push_back(body.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
    if (at == std::string_view::npos) {
      break;
    }
    start = at + 1;
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (it->empty()) {
      continue;
    }
    if (!out.empty()) {
      out += "::";
    }
    out += *it;
  }
  return out.empty() ? std::string(body) : out;
}

}  // namespace skydiag::rtti
