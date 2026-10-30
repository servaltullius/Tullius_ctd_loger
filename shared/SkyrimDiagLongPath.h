#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace skydiag {

// Win32 file calls reject a path of MAX_PATH (260) characters or more, and a
// directory of 248 or more, unless the process opts into long paths, which
// these EXEs and DLLs do not. The extended-length form ("\\?\C:\...",
// "\\?\UNC\server\share\...") has no such limit, but it also skips Win32's
// path clean-up, so the path is made absolute first:
// std::filesystem::absolute calls GetFullPathNameW, the same clean-up the
// plain calls apply ("/" to "\", "." and "..", trailing dots and spaces). An
// empty path, a device path or one that cannot be made absolute comes back
// as is, as does every path off Windows.
inline std::filesystem::path ExtendedLengthPath(const std::filesystem::path& path)
{
#ifdef _WIN32
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return path;
  }
  const std::wstring& text = absolute.native();
  if (text.starts_with(LR"(\\?\)") || text.starts_with(LR"(\\.\)")) {
    return absolute;
  }
  if (text.starts_with(LR"(\\)")) {
    return std::filesystem::path(LR"(\\?\UNC\)" + text.substr(2));
  }
  if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') {
    return std::filesystem::path(LR"(\\?\)" + text);
  }
  return path;
#else
  return path;
#endif
}

// The longest path every plain Win32 call takes: a directory must stay under
// 248 characters (MAX_PATH less room for an 8.3 file name).
inline constexpr std::size_t kMaxPlainPathChars = 247;

// The path to hand a file call: the path itself while it, with `extra` more
// characters below it (a temporary suffix, the deepest file of a tree), stays
// within kMaxPlainPathChars, else its extended-length form. Only long paths
// change. The usual ones reach Windows exactly as before, which matters under
// MO2: its virtual file system has not been checked with extended-length
// paths, and the extended form of a volume root is not a directory to
// create_directories.
inline std::filesystem::path PathForIo(const std::filesystem::path& path, std::size_t extra = 0)
{
#ifdef _WIN32
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec || absolute.native().size() + extra <= kMaxPlainPathChars) {
    return path;
  }
  return ExtendedLengthPath(absolute);
#else
  static_cast<void>(extra);
  return path;
#endif
}

// The path without an extended-length prefix: for command lines and messages.
// The programs started with it apply PathForIo themselves.
inline std::filesystem::path PlainPath(const std::filesystem::path& path)
{
#ifdef _WIN32
  const std::wstring& text = path.native();
  if (text.starts_with(LR"(\\?\UNC\)")) {
    return std::filesystem::path(LR"(\\)" + text.substr(8));
  }
  if (text.starts_with(LR"(\\?\)") && text.size() >= 6 && text[5] == L':') {
    return std::filesystem::path(text.substr(4));
  }
#endif
  return path;
}

// create_directories on the extended-length form of a volume or share root
// ("\\?\D:\", "\\?\UNC\server\share\") fails with access denied where the
// plain "D:\" succeeds, because MSVC asks CreateDirectoryW for the root
// itself. PathForIo never makes a root extended, but a caller may pass one
// (--out-dir "\\?\D:\"), and a directory that already exists is what callers
// want, so that is no error.
inline bool CreateDirectoriesForIo(const std::filesystem::path& dir, std::error_code& ec)
{
  const auto ioDir = PathForIo(dir);
  std::filesystem::create_directories(ioDir, ec);
  if (ec) {
    std::error_code existsEc;
    if (std::filesystem::is_directory(ioDir, existsEc)) {
      ec.clear();
    }
  }
  return !ec;
}

}  // namespace skydiag
