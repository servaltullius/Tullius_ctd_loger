#include <string_view>

#include <SKSE/SKSE.h>

using namespace std::literals;

SKSEPluginInfo(
  .Version = REL::Version{ 0, 2, 11, 0 },
  .Name = "SkyrimDiag"sv,
  .Author = ""sv,
  .SupportEmail = ""sv,
  .StructCompatibility = SKSE::StructCompatibility::Independent,
  .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
  .MinimumSKSEVersion = REL::Version{ 0, 0, 0, 0 })
