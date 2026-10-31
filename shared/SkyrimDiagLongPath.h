#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
// MO2: its virtual file system loses some calls on long paths even in the
// extended form (see below), and the extended form of a volume root is not
// a directory to create_directories.
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

// Under MO2 (2.5.2, measured 2026-10-31), the virtual file system answers a
// program that is not long-path aware, as none of ours is, with "path not
// found" from GetFileAttributes(Ex)W, FindFirstFileExW, DeleteFileW and
// RemoveDirectoryW for a path past MAX_PATH, even in the extended-length
// form, while CreateFileW, CreateDirectoryW, MoveFileExW and queries on an
// open handle work. So there std::filesystem's status, exists, is_*,
// last_write_time, directory_iterator and remove_all fail on such a path,
// and so does create_directories once the folder exists; remove works (it
// deletes through a handle). For an extended-length path the helpers below
// go through a handle; for any other path they are std::filesystem as
// before.
namespace long_path_detail {

#ifdef _WIN32
inline bool IsExtended(const std::filesystem::path& path)
{
  return path.native().starts_with(LR"(\\?\)");
}

// Opened for its attributes only, which no other opener's sharing mode
// refuses, with backup semantics so that a directory opens too.
inline bool QueryByHandle(const std::filesystem::path& path, BY_HANDLE_FILE_INFORMATION* info, DWORD flags = 0)
{
  const HANDLE handle = CreateFileW(
    path.c_str(),
    FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | flags,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const BOOL ok = GetFileInformationByHandle(handle, info);
  CloseHandle(handle);
  return ok != FALSE;
}

inline bool IsDirectoryByHandle(const std::filesystem::path& path)
{
  BY_HANDLE_FILE_INFORMATION info{};
  return QueryByHandle(path, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// MSVC's file_time_type counts FILETIME ticks: 100 ns since 1601.
inline std::filesystem::file_time_type FileTimeFromTicks(LONGLONG ticks)
{
  return std::filesystem::file_time_type(std::filesystem::file_time_type::duration(ticks));
}

// Calls visit(name, attributes, size, lastWrite) for every entry of a
// directory but "." and "..". False, with the Win32 error, when the
// directory cannot be opened or read to the end.
template <class Visit>
bool EnumerateByHandle(const std::filesystem::path& dir, Visit&& visit, DWORD* error)
{
  const HANDLE handle = CreateFileW(
    dir.c_str(),
    FILE_LIST_DIRECTORY | SYNCHRONIZE,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  std::vector<std::uint64_t> buffer(8192);  // 64 KiB, aligned for the records
  const auto bufferBytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
  FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdBothDirectoryRestartInfo;
  bool ok = true;
  while (GetFileInformationByHandleEx(handle, infoClass, buffer.data(), bufferBytes)) {
    infoClass = FileIdBothDirectoryInfo;
    const auto* record = reinterpret_cast<const unsigned char*>(buffer.data());
    for (;;) {
      const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(record);
      const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        visit(
          name,
          info->FileAttributes,
          static_cast<std::uintmax_t>(info->EndOfFile.QuadPart),
          FileTimeFromTicks(info->LastWriteTime.QuadPart));
      }
      if (info->NextEntryOffset == 0) {
        break;
      }
      record += info->NextEntryOffset;
    }
  }
  const DWORD stop = GetLastError();
  if (stop != ERROR_NO_MORE_FILES) {
    *error = stop;
    ok = false;
  }
  CloseHandle(handle);
  return ok;
}

// Where the folders below the root start: after "\\?\D:\", after
// "\\?\UNC\server\share\", after "\\?\Volume{...}\".
inline std::size_t RootLength(const std::wstring& text)
{
  if (text.size() >= 6 && text[5] == L':') {
    return text.size() < 7 ? text.size() : 7;
  }
  std::size_t pos = 4;
  int separators = text.starts_with(LR"(\\?\UNC\)") ? 3 : 1;
  for (; pos < text.size() && separators > 0; ++pos) {
    if (text[pos] == L'\\') {
      --separators;
    }
  }
  return pos;
}

inline std::uintmax_t RemoveTreeByHandle(const std::filesystem::path& dir, std::error_code& ec)
{
  std::vector<std::pair<std::filesystem::path, bool>> children;
  DWORD error = ERROR_SUCCESS;
  const bool listed = EnumerateByHandle(
    dir,
    [&](std::wstring_view name, DWORD attributes, std::uintmax_t, std::filesystem::file_time_type) {
      // A link to a folder is removed itself, not followed, as by remove_all.
      const bool tree = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
      children.emplace_back(dir / std::wstring(name), tree);
    },
    &error);
  if (!listed) {
    ec.assign(static_cast<int>(error), std::system_category());
    return 0;
  }
  std::uintmax_t removed = 0;
  for (const auto& [child, tree] : children) {
    if (tree) {
      removed += RemoveTreeByHandle(child, ec);
    } else if (std::filesystem::remove(child, ec)) {
      ++removed;
    }
    if (ec) {
      return removed;
    }
  }
  if (std::filesystem::remove(dir, ec)) {
    ++removed;
  }
  return removed;
}
#endif

}  // namespace long_path_detail

inline bool IsDirectoryForIo(const std::filesystem::path& path)
{
  const auto ioPath = PathForIo(path);
#ifdef _WIN32
  if (long_path_detail::IsExtended(ioPath)) {
    return long_path_detail::IsDirectoryByHandle(ioPath);
  }
#endif
  std::error_code ec;
  return std::filesystem::is_directory(ioPath, ec);
}

inline bool IsRegularFileForIo(const std::filesystem::path& path)
{
  const auto ioPath = PathForIo(path);
#ifdef _WIN32
  if (long_path_detail::IsExtended(ioPath)) {
    BY_HANDLE_FILE_INFORMATION info{};
    return long_path_detail::QueryByHandle(ioPath, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  }
#endif
  std::error_code ec;
  return std::filesystem::is_regular_file(ioPath, ec);
}

struct DirectoryEntryForIo
{
  std::filesystem::path path;  // in the form the directory was listed in
  bool directory = false;
  std::uintmax_t size = 0;  // of a file
  std::optional<std::filesystem::file_time_type> lastWrite;  // empty when unknown
};

// The entries of a directory; ec tells a listing that failed from an empty
// directory.
inline std::vector<DirectoryEntryForIo> ListDirectoryForIo(const std::filesystem::path& dir, std::error_code& ec)
{
  ec.clear();
  std::vector<DirectoryEntryForIo> entries;
  // The listing reads "<dir>\*".
  const auto ioDir = PathForIo(dir, 2);
#ifdef _WIN32
  if (long_path_detail::IsExtended(ioDir)) {
    DWORD error = ERROR_SUCCESS;
    const bool listed = long_path_detail::EnumerateByHandle(
      ioDir,
      [&](std::wstring_view name, DWORD attributes, std::uintmax_t size, std::filesystem::file_time_type lastWrite) {
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.push_back({ ioDir / std::wstring(name), directory, directory ? std::uintmax_t{ 0 } : size, lastWrite });
      },
      &error);
    if (!listed) {
      ec.assign(static_cast<int>(error), std::system_category());
    }
    return entries;
  }
#endif
  // increment(ec), not the range-for's operator++, which throws when the
  // folder becomes unreadable mid-scan (a removed drive).
  for (std::filesystem::directory_iterator it(ioDir, ec), end; !ec && it != end; it.increment(ec)) {
    DirectoryEntryForIo entry;
    entry.path = it->path();
    std::error_code entryEc;
    entry.directory = it->is_directory(entryEc);
    if (!entry.directory) {
      const auto size = it->file_size(entryEc);
      entry.size = entryEc ? 0 : size;
    }
    const auto written = it->last_write_time(entryEc);
    if (!entryEc) {
      entry.lastWrite = written;
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

// remove_all: a link is removed, not followed; a path that is not there
// removes nothing and is no error. A tree whose deepest path passes MAX_PATH
// is passed in its extended form (PathForIo with the depth as `extra`).
inline std::uintmax_t RemoveAllForIo(const std::filesystem::path& path, std::error_code& ec)
{
  ec.clear();
  const auto ioPath = PathForIo(path);
#ifdef _WIN32
  if (long_path_detail::IsExtended(ioPath)) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!long_path_detail::QueryByHandle(ioPath, &info, FILE_FLAG_OPEN_REPARSE_POINT)) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        ec.assign(static_cast<int>(error), std::system_category());
      }
      return 0;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      return long_path_detail::RemoveTreeByHandle(ioPath, ec);
    }
    return std::filesystem::remove(ioPath, ec) ? 1 : 0;
  }
#endif
  return std::filesystem::remove_all(ioPath, ec);
}

// create_directories, and a directory that already exists is no error. On
// an extended-length path each folder from the root down is made by
// CreateDirectoryW, and one that exists is checked through a handle (above).
// That also covers the extended form of a volume or share root ("\\?\D:\",
// "\\?\UNC\server\share\"), which create_directories fails with access
// denied, because MSVC asks CreateDirectoryW for the root itself: PathForIo
// never makes a root extended, but a caller may pass one (--out-dir
// "\\?\D:\").
inline bool CreateDirectoriesForIo(const std::filesystem::path& dir, std::error_code& ec)
{
  ec.clear();
  const auto ioDir = PathForIo(dir);
#ifdef _WIN32
  if (long_path_detail::IsExtended(ioDir)) {
    const std::wstring& text = ioDir.native();
    const std::size_t root = long_path_detail::RootLength(text);
    for (std::size_t pos = root; pos < text.size();) {
      std::size_t next = text.find(L'\\', pos);
      if (next == std::wstring::npos) {
        next = text.size();
      }
      if (next > pos) {
        const std::wstring folder = text.substr(0, next);
        if (!CreateDirectoryW(folder.c_str(), nullptr)) {
          const DWORD error = GetLastError();
          if (!long_path_detail::IsDirectoryByHandle(folder)) {
            ec.assign(static_cast<int>(error), std::system_category());
            return false;
          }
        }
      }
      pos = next + 1;
    }
    // "\\?\D:" is the volume; its root folder is "\\?\D:\".
    const bool bareDrive = text.size() == 6 && text[5] == L':';
    if (!long_path_detail::IsDirectoryByHandle(bareDrive ? std::filesystem::path(text + L"\\") : ioDir)) {
      ec = std::make_error_code(std::errc::not_a_directory);
      return false;
    }
    return true;
  }
#endif
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
