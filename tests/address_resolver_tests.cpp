#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static std::string ReadFile(const char* relPath)
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  const std::filesystem::path p = std::filesystem::path(root) / relPath;
  std::ifstream in(p, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestAddressDbExists()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root);
  const std::filesystem::path p = std::filesystem::path(root) / "dump_tool" / "data" / "address_db" / "skyrimse_functions.json";
  assert(std::filesystem::exists(p));
}

static void TestAddressResolverApiExists()
{
  const auto header = ReadFile("dump_tool/src/AddressResolver.h");
  assert(header.find("AddressResolver") != std::string::npos);
  assert(header.find("LoadFromJson") != std::string::npos);
  assert(header.find("Resolve") != std::string::npos);
}

static void TestKnownFunctionsPresent()
{
  const auto content = ReadFile("dump_tool/data/address_db/skyrimse_functions.json");
  assert(content.find("BSBatchRenderer::Draw") != std::string::npos);
}

int main()
{
  TestAddressDbExists();
  TestAddressResolverApiExists();
  TestKnownFunctionsPresent();
  return 0;
}
