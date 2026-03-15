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
    private sealed record HelperLayout(string HelperDirectoryPath, string HelperIniPath);

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

    public static bool CanPromoteLearnedRoot(
        DumpDiscoveryState state,
        string dumpPath)
    {
        if (string.IsNullOrWhiteSpace(dumpPath))
        {
            return false;
        }

        string? directoryPath;
        try
        {
            directoryPath = Path.GetDirectoryName(Path.GetFullPath(dumpPath));
        }
        catch
        {
            return false;
        }

        if (string.IsNullOrWhiteSpace(directoryPath))
        {
            return false;
        }

        foreach (var root in BuildSearchRoots(state, currentDumpPath: null, isKorean: false))
        {
            if (IsPathWithinRoot(directoryPath, root.Path))
            {
                return true;
            }
        }

        return false;
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
            addRoot(root, DumpSearchRootKind.Learned, isKorean ? "최근 성공 출력 위치" : "Recent output location", true);
        }

        foreach (var root in state.RegisteredRoots)
        {
            addRoot(root, DumpSearchRootKind.Registered, isKorean ? "등록된 출력 위치" : "Saved output location", true);
        }

        foreach (var automaticRoot in BuildAutomaticOutputRoots(isKorean))
        {
            addRoot(automaticRoot.Path, automaticRoot.Kind, automaticRoot.SourceLabel, automaticRoot.IsRemovable);
        }

        return roots;
    }

    private static IReadOnlyList<DumpSearchRoot> BuildAutomaticOutputRoots(bool isKorean)
    {
        if (!TryResolveHelperLayout(out var layout))
        {
            return Array.Empty<DumpSearchRoot>();
        }

        if (TryResolveConfiguredOutputRoot(layout, out var configuredOutputRoot))
        {
            return new[]
            {
                new DumpSearchRoot(
                    configuredOutputRoot,
                    DumpSearchRootKind.Automatic,
                    isKorean ? "설정된 OutputDir" : "Configured OutputDir",
                    false),
            };
        }

        if (TryResolveDefaultOutputRoot(layout, isKorean, out var defaultOutputRoot, out var sourceLabel))
        {
            return new[]
            {
                new DumpSearchRoot(defaultOutputRoot, DumpSearchRootKind.Automatic, sourceLabel, false),
            };
        }

        return Array.Empty<DumpSearchRoot>();
    }

    private static bool TryResolveHelperLayout(out HelperLayout layout)
    {
        layout = default!;

        string baseDirectory;
        try
        {
            baseDirectory = Path.GetFullPath(AppContext.BaseDirectory);
        }
        catch
        {
            return false;
        }

        var helperDirectory = Path.GetFullPath(Path.Combine(baseDirectory, ".."));
        var helperIniPath = Path.Combine(helperDirectory, "SkyrimDiagHelper.ini");
        if (!File.Exists(helperIniPath))
        {
            return false;
        }

        layout = new HelperLayout(helperDirectory, helperIniPath);
        return true;
    }

    private static bool TryResolveConfiguredOutputRoot(HelperLayout layout, out string outputRoot)
    {
        outputRoot = string.Empty;

        var configuredValue = TryReadIniValue(layout.HelperIniPath, "SkyrimDiagHelper", "OutputDir");
        if (string.IsNullOrWhiteSpace(configuredValue))
        {
            return false;
        }

        var trimmed = configuredValue.Trim().Trim('"');
        if (string.IsNullOrWhiteSpace(trimmed))
        {
            return false;
        }

        try
        {
            outputRoot = Path.IsPathRooted(trimmed)
                ? Path.GetFullPath(trimmed)
                : Path.GetFullPath(Path.Combine(layout.HelperDirectoryPath, trimmed));
            return true;
        }
        catch
        {
            outputRoot = string.Empty;
            return false;
        }
    }

    private static bool TryResolveDefaultOutputRoot(
        HelperLayout layout,
        bool isKorean,
        out string outputRoot,
        out string sourceLabel)
    {
        outputRoot = string.Empty;
        sourceLabel = string.Empty;

        if (TryInferMo2BaseDirectory(layout.HelperDirectoryPath, out var mo2BaseDirectory))
        {
            outputRoot = Path.Combine(mo2BaseDirectory, "overwrite", "SKSE", "Plugins");
            sourceLabel = "MO2 overwrite";
            return true;
        }

        outputRoot = layout.HelperDirectoryPath;
        sourceLabel = isKorean ? "기본 출력 위치" : "Default output folder";
        return true;
    }

    private static bool TryInferMo2BaseDirectory(string helperDirectoryPath, out string mo2BaseDirectory)
    {
        mo2BaseDirectory = string.Empty;

        DirectoryInfo? current;
        try
        {
            current = new DirectoryInfo(helperDirectoryPath);
        }
        catch
        {
            return false;
        }

        while (current is not null)
        {
            if (string.Equals(current.Name, "mods", StringComparison.OrdinalIgnoreCase) &&
                current.Parent is not null)
            {
                mo2BaseDirectory = current.Parent.FullName;
                return true;
            }

            current = current.Parent;
        }

        return false;
    }

    private static string? TryReadIniValue(string iniPath, string sectionName, string keyName)
    {
        string? currentSection = null;
        IEnumerable<string> lines;
        try
        {
            lines = File.ReadLines(iniPath);
        }
        catch
        {
            return null;
        }

        foreach (var rawLine in lines)
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('#'))
            {
                continue;
            }

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                currentSection = line[1..^1].Trim();
                continue;
            }

            if (!string.Equals(currentSection, sectionName, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var separatorIndex = line.IndexOf('=');
            if (separatorIndex <= 0)
            {
                continue;
            }

            var key = line[..separatorIndex].Trim();
            if (!string.Equals(key, keyName, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return line[(separatorIndex + 1)..].Trim();
        }

        return null;
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

    private static bool IsPathWithinRoot(string candidatePath, string rootPath)
    {
        try
        {
            var normalizedCandidate = Path.GetFullPath(candidatePath)
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var normalizedRoot = Path.GetFullPath(rootPath)
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

            if (string.Equals(normalizedCandidate, normalizedRoot, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            var rootWithSeparator = normalizedRoot + Path.DirectorySeparatorChar;
            return normalizedCandidate.StartsWith(rootWithSeparator, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
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
