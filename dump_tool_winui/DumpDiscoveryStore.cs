using System.Text.Json;

namespace SkyrimDiagDumpToolWinUI;

internal sealed class DumpDiscoveryState
{
    public int Version { get; init; } = 1;
    public List<string> RegisteredRoots { get; init; } = new();
    public List<string> LearnedRoots { get; init; } = new();
}

internal static class DumpDiscoveryStore
{
    private static readonly JsonSerializerOptions s_writeOptions = new()
    {
        WriteIndented = true,
    };

    public static DumpDiscoveryState Load()
    {
        try
        {
            var path = GetStatePath();
            if (!File.Exists(path))
            {
                return new DumpDiscoveryState();
            }

            var json = File.ReadAllText(path);
            var loaded = JsonSerializer.Deserialize<DumpDiscoveryState>(json) ?? new DumpDiscoveryState();
            return Sanitize(loaded);
        }
        catch
        {
            return new DumpDiscoveryState();
        }
    }

    public static async Task SaveAsync(DumpDiscoveryState state, CancellationToken cancellationToken)
    {
        var path = GetStatePath();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var json = JsonSerializer.Serialize(Sanitize(state), s_writeOptions) + Environment.NewLine;
        await File.WriteAllTextAsync(path, json, cancellationToken);
    }

    public static DumpDiscoveryState AddRegisteredRoot(DumpDiscoveryState state, string root)
    {
        var normalized = NormalizeRoot(root);
        if (string.IsNullOrWhiteSpace(normalized))
        {
            return Sanitize(state);
        }

        return Sanitize(new DumpDiscoveryState
        {
            Version = 1,
            RegisteredRoots = new[] { normalized }.Concat(state.RegisteredRoots).ToList(),
            LearnedRoots = state.LearnedRoots,
        });
    }

    public static DumpDiscoveryState PromoteLearnedRoot(DumpDiscoveryState state, string root)
    {
        var normalized = NormalizeRoot(root);
        if (string.IsNullOrWhiteSpace(normalized))
        {
            return Sanitize(state);
        }

        return Sanitize(new DumpDiscoveryState
        {
            Version = 1,
            RegisteredRoots = state.RegisteredRoots,
            LearnedRoots = new[] { normalized }.Concat(state.LearnedRoots).ToList(),
        });
    }

    public static DumpDiscoveryState RemoveRoot(DumpDiscoveryState state, string root)
    {
        var normalized = NormalizeRoot(root);
        if (string.IsNullOrWhiteSpace(normalized))
        {
            return Sanitize(state);
        }

        return Sanitize(new DumpDiscoveryState
        {
            Version = 1,
            RegisteredRoots = state.RegisteredRoots
                .Where(path => !string.Equals(NormalizeRoot(path), normalized, StringComparison.OrdinalIgnoreCase))
                .ToList(),
            LearnedRoots = state.LearnedRoots
                .Where(path => !string.Equals(NormalizeRoot(path), normalized, StringComparison.OrdinalIgnoreCase))
                .ToList(),
        });
    }

    private static DumpDiscoveryState Sanitize(DumpDiscoveryState state)
    {
        return new DumpDiscoveryState
        {
            Version = 1,
            RegisteredRoots = NormalizeList(state.RegisteredRoots),
            LearnedRoots = NormalizeList(state.LearnedRoots),
        };
    }

    private static List<string> NormalizeList(IEnumerable<string> roots)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var result = new List<string>();

        foreach (var root in roots)
        {
            var normalized = NormalizeRoot(root);
            if (string.IsNullOrWhiteSpace(normalized) || IsLegacyExcludedRoot(normalized) || !seen.Add(normalized))
            {
                continue;
            }

            result.Add(normalized);
            if (result.Count >= 12)
            {
                break;
            }
        }

        return result;
    }

    private static bool IsLegacyExcludedRoot(string normalizedRoot)
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(localAppData))
        {
            return false;
        }

        string crashDumpsRoot;
        try
        {
            crashDumpsRoot = Path.GetFullPath(Path.Combine(localAppData, "CrashDumps"))
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
        catch
        {
            return false;
        }

        return string.Equals(normalizedRoot, crashDumpsRoot, StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeRoot(string? root)
    {
        if (string.IsNullOrWhiteSpace(root))
        {
            return string.Empty;
        }

        try
        {
            return Path.GetFullPath(root.Trim())
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string GetStatePath()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return Path.Combine(localAppData, "TulliusCtdLogger", "dump-discovery.json");
    }
}
