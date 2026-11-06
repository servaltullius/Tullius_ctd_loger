#pragma once

// What an AI package (TESPackage) a crash or freeze was handling is made of:
// its type, the package it was made from, and its data entries. A package the
// game made while it ran (FormID FFxxxxxx) has no plugin of its own; the
// package it was copied from (the template) names the plugin.
//
// Shared by the helper and the analyzer, as SkyrimDiagRtti.h is: the helper
// runs it over the live game for every package it finds, and adds every
// range read to the dump; the analyzer runs it over the dump. The reader is
// all-or-nothing: bool read(std::uint64_t address, void* destination, std::size_t size).
//
// Layouts (CommonLibSSE-NG, SE/AE):
// - TESPackage (0xE0): PACKAGE_DATA packData at 0x20 with packType at +4
//   (0x24); TESPackageData* data at 0x30.
// - TESCustomPackageData (0x40, a package made in the Creation Kit's package
//   editor): BGSPackageDataList data at 0x08 = { IPackageData** data (0x08),
//   ..., std::uint16_t dataSize (0x18) }; TESPackage* templateParent at 0x30.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SkyrimDiagRtti.h"

namespace skydiag::package {

inline constexpr std::string_view kTesPackageTypeName = ".?AVTESPackage@@";
inline constexpr std::string_view kCustomPackageDataTypeName = ".?AVTESCustomPackageData@@";
inline constexpr std::uint64_t kPackTypeOffset = 0x24;
inline constexpr std::uint64_t kPackageDataOffset = 0x30;
inline constexpr std::uint64_t kCustomDataArrayOffset = 0x08;
inline constexpr std::uint64_t kCustomDataCountOffset = 0x18;
inline constexpr std::uint64_t kCustomTemplateOffset = 0x30;
// More data entries than any package the game ships; past it the count is
// not believed.
inline constexpr std::uint16_t kMaxDataEntries = 32;

struct DataEntry
{
  std::uint64_t address = 0;
  std::string type_name;  // decorated; empty when the entry is not an object
};

struct PackageLinks
{
  std::uint8_t pack_type = 0;  // PACKAGE_PROCEDURE_TYPE: 18 a package made from a template
  bool has_custom_data = false;
  std::optional<rtti::ObjectInfo> template_package;
  std::uint16_t data_count = 0;  // as the package says
  std::vector<DataEntry> data;   // the entries read, up to kMaxDataEntries
};

inline bool IsPackage(const rtti::ObjectInfo& object)
{
  for (const auto& base : object.base_names) {
    if (base == kTesPackageTypeName) {
      return true;
    }
  }
  return object.type_name == kTesPackageTypeName;
}

// Follows a package's links from its complete object. None when the package
// itself cannot be read; whatever of its data could be read otherwise.
template <class Reader>
std::optional<PackageLinks> ReadPackageLinks(const Reader& read, std::uint64_t package)
{
  using rtti::detail::PlausiblePointer;
  using rtti::detail::ReadValue;
  PackageLinks links{};
  std::uint64_t data = 0;
  if (!ReadValue(read, package + kPackTypeOffset, links.pack_type) ||
      !ReadValue(read, package + kPackageDataOffset, data)) {
    return std::nullopt;
  }
  if (!PlausiblePointer(data)) {
    return links;
  }
  const auto custom = rtti::ReadObject(read, data);
  if (!custom || custom->type_name != kCustomPackageDataTypeName) {
    return links;
  }
  links.has_custom_data = true;
  std::uint64_t templatePackage = 0;
  if (ReadValue(read, data + kCustomTemplateOffset, templatePackage) && PlausiblePointer(templatePackage)) {
    if (auto object = rtti::ReadObject(read, templatePackage); object && IsPackage(*object)) {
      links.template_package = std::move(object);
    }
  }
  std::uint64_t array = 0;
  if (!ReadValue(read, data + kCustomDataArrayOffset, array) ||
      !ReadValue(read, data + kCustomDataCountOffset, links.data_count) || !PlausiblePointer(array)) {
    return links;
  }
  const std::uint16_t count = links.data_count < kMaxDataEntries ? links.data_count : kMaxDataEntries;
  std::vector<std::uint64_t> entries(count);
  if (count != 0u && !read(array, entries.data(), entries.size() * sizeof(std::uint64_t))) {
    return links;
  }
  for (const auto entry : entries) {
    DataEntry item{};
    item.address = entry;
    if (const auto object = rtti::ReadObject(read, entry)) {
      item.type_name = object->type_name;
    }
    links.data.push_back(std::move(item));
  }
  return links;
}

}  // namespace skydiag::package
