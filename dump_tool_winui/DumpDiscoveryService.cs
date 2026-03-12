namespace SkyrimDiagDumpToolWinUI;

internal static class DumpDiscoveryService
{
    private enum DumpSearchRootKind
    {
        Learned,
        Registered,
        Automatic,
    }

    private sealed record DumpSearchRoot(string Path, DumpSearchRootKind Kind, string SourceLabel, bool IsRemovable);
    private sealed record DumpFileCandidate(string FullPath, string DirectoryPath, string SourceLabel, DateTime LastWriteTimeLocal, long SizeBytes);

    public static IReadOnlyList<DumpDiscoveryItem> DiscoverRecentDumps(
        DumpDiscoveryState state,
        string? currentDumpPath,
        bool isKorean,
        int maxResults = 12)
    {
        var discovered = new List<DumpFileCandidate>();
        var seenFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var root in BuildSearchRoots(state, currentDumpPath, isKorean))
        {
            if (!Directory.Exists(root.Path))
            {
                continue;
            }

            foreach (var file in EnumerateDumpFiles(root.Path, maxDirectories: 256, maxFiles: 96))
            {
                string fullPath;
                try
                {
                    fullPath = Path.GetFullPath(file);
                }
                catch
                {
                    continue;
                }

                if (!seenFiles.Add(fullPath))
                {
                    continue;
                }

                try
                {
                    var info = new FileInfo(fullPath);
                    discovered.Add(new DumpFileCandidate(
                        fullPath,
                        info.DirectoryName ?? root.Path,
                        root.SourceLabel,
                        info.LastWriteTime,
                        info.Length));
                }
                catch
                {
                    // Ignore files that disappear during scanning.
                }
            }
        }

        return discovered
            .OrderByDescending(item => item.LastWriteTimeLocal)
            .Take(maxResults)
            .Select(item => new DumpDiscoveryItem(
                Path.GetFileName(item.FullPath),
                item.FullPath,
                item.SourceLabel,
                item.DirectoryPath,
                item.LastWriteTimeLocal.ToString("yyyy-MM-dd HH:mm"),
                FormatSize(item.SizeBytes),
                isKorean ? "분석" : "Analyze"))
            .ToList();
    }

    public static IReadOnlyList<DumpSearchLocationItem> BuildSearchLocationItems(
        DumpDiscoveryState state,
        string? currentDumpPath,
        bool isKorean)
    {
        return BuildSearchRoots(state, currentDumpPath, isKorean)
            .Select(root => new DumpSearchLocationItem(root.Path, root.SourceLabel, root.IsRemovable))
            .ToList();
    }

    private static IReadOnlyList<DumpSearchRoot> BuildSearchRoots(
        DumpDiscoveryState state,
        string? currentDumpPath,
        bool isKorean)
    {
        var roots = new List<DumpSearchRoot>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void addRoot(string path, DumpSearchRootKind kind, string sourceLabel, bool isRemovable)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            string fullPath;
            try
            {
                fullPath = Path.GetFullPath(path);
            }
            catch
            {
                return;
            }

            var normalized = fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!seen.Add(normalized))
            {
                return;
            }

            roots.Add(new DumpSearchRoot(normalized, kind, sourceLabel, isRemovable));
        }

        foreach (var root in state.LearnedRoots)
        {
            addRoot(root, DumpSearchRootKind.Learned, isKorean ? "최근 성공 위치" : "Recent success", true);
        }

        foreach (var root in state.RegisteredRoots)
        {
            addRoot(root, DumpSearchRootKind.Registered, isKorean ? "등록 경로" : "Saved search root", true);
        }

        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!string.IsNullOrWhiteSpace(localAppData))
        {
            addRoot(Path.Combine(localAppData, "CrashDumps"), DumpSearchRootKind.Automatic, "CrashDumps", false);
        }

        if (!string.IsNullOrWhiteSpace(currentDumpPath))
        {
            try
            {
                var currentDirectory = Path.GetDirectoryName(Path.GetFullPath(currentDumpPath));
                if (!string.IsNullOrWhiteSpace(currentDirectory))
                {
                    addRoot(currentDirectory, DumpSearchRootKind.Automatic, isKorean ? "현재 덤프 위치" : "Current dump folder", false);
                }
            }
            catch
            {
                // Ignore malformed input paths.
            }
        }

        return roots;
    }

    private static IEnumerable<string> EnumerateDumpFiles(string root, int maxDirectories, int maxFiles)
    {
        var pending = new Stack<string>();
        pending.Push(root);

        var visitedDirectories = 0;
        var yieldedFiles = 0;

        while (pending.Count > 0 && visitedDirectories < maxDirectories && yieldedFiles < maxFiles)
        {
            var current = pending.Pop();
            visitedDirectories++;

            IEnumerable<string> files;
            try
            {
                files = Directory.EnumerateFiles(current, "*.dmp", SearchOption.TopDirectoryOnly);
            }
            catch
            {
                continue;
            }

            foreach (var file in files)
            {
                yield return file;
                yieldedFiles++;
                if (yieldedFiles >= maxFiles)
                {
                    yield break;
                }
            }

            IEnumerable<string> subdirectories;
            try
            {
                subdirectories = Directory.EnumerateDirectories(current);
            }
            catch
            {
                continue;
            }

            foreach (var subdirectory in subdirectories)
            {
                pending.Push(subdirectory);
            }
        }
    }

    private static string FormatSize(long sizeBytes)
    {
        if (sizeBytes >= 1024 * 1024)
        {
            return $"{sizeBytes / (1024.0 * 1024.0):F1} MB";
        }

        if (sizeBytes >= 1024)
        {
            return $"{sizeBytes / 1024.0:F1} KB";
        }

        return $"{sizeBytes} B";
    }
}
