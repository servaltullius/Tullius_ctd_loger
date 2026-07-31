using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Globalization;

namespace SkyrimDiagDumpToolWinUI;

internal sealed record DumpIdentityContract(
    string Sha256,
    long SizeBytes,
    long LastWriteTimeUtc100ns)
{
    public static DumpIdentityContract Invalid { get; } = new(string.Empty, 0, 0);

    public bool IsValid =>
        Sha256.Length == 64 &&
        Sha256.All(static ch =>
            ch is >= '0' and <= '9' ||
            ch is >= 'a' and <= 'f') &&
        SizeBytes > 0 &&
        LastWriteTimeUtc100ns > 0;

    public static DumpIdentityContract FromJson(JsonElement root)
    {
        var node = root;
        if (root.ValueKind == JsonValueKind.Object &&
            root.TryGetProperty("dump_identity", out var nested))
        {
            node = nested;
        }
        if (node.ValueKind != JsonValueKind.Object ||
            !node.TryGetProperty("schema", out var schema) ||
            schema.GetString() != "skydiag.dump_identity.v1" ||
            !node.TryGetProperty("sha256", out var sha) ||
            !node.TryGetProperty("size_bytes", out var size) ||
            !node.TryGetProperty("last_write_time_utc_100ns", out var modified) ||
            !size.TryGetInt64(out var sizeValue) ||
            !modified.TryGetInt64(out var modifiedValue))
        {
            return Invalid;
        }

        var parsed = new DumpIdentityContract(
            (sha.GetString() ?? string.Empty).Trim().ToLowerInvariant(),
            sizeValue,
            modifiedValue);
        return parsed.IsValid ? parsed : Invalid;
    }

    public async Task<bool> MatchesFileAsync(string dumpPath, CancellationToken cancellationToken)
    {
        if (!IsValid)
        {
            return false;
        }

        var fullPath = Path.GetFullPath(dumpPath);
        var before = ReadMetadata(fullPath);
        if (before.size != SizeBytes || before.modified != LastWriteTimeUtc100ns)
        {
            return false;
        }

        var computed = await ComputeAsync(fullPath, cancellationToken);
        return this == computed;
    }

    public static async Task<DumpIdentityContract> ComputeAsync(
        string dumpPath,
        CancellationToken cancellationToken)
    {
        var fullPath = Path.GetFullPath(dumpPath);
        var before = ReadMetadata(fullPath);
        await using var stream = new FileStream(
            fullPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            1024 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var digest = await SHA256.HashDataAsync(stream, cancellationToken);
        var after = ReadMetadata(fullPath);
        if (before != after)
        {
            throw new IOException("Dump changed while its identity was being verified.");
        }

        return new DumpIdentityContract(
            Convert.ToHexString(digest).ToLowerInvariant(),
            before.size,
            before.modified);
    }

    public JsonObject ToJsonObject() => new()
    {
        ["schema"] = "skydiag.dump_identity.v1",
        ["sha256"] = Sha256,
        ["size_bytes"] = SizeBytes,
        ["last_write_time_utc_100ns"] = LastWriteTimeUtc100ns,
    };

    public string StorageMetadataKey =>
        IsValid
            ? SizeBytes.ToString("x16", CultureInfo.InvariantCulture) + "." +
              LastWriteTimeUtc100ns.ToString("x16", CultureInfo.InvariantCulture)
            : string.Empty;

    public string ResolveArtifactDirectory(string outDir)
    {
        if (!IsValid)
        {
            throw new InvalidOperationException("A valid dump identity is required.");
        }
        return Path.Combine(
            Path.GetFullPath(outDir),
            ".skydiag-analysis",
            Sha256,
            StorageMetadataKey);
    }

    public string ResolveTriageStatePath(string outDir) =>
        Path.Combine(
            Path.GetFullPath(outDir),
            ".skydiag-triage",
            Sha256,
            StorageMetadataKey + ".json");

    private static (long size, long modified) ReadMetadata(string path)
    {
        var info = new FileInfo(path);
        info.Refresh();
        if (!info.Exists || info.Length <= 0)
        {
            throw new FileNotFoundException("Dump file is missing or empty.", path);
        }
        return (info.Length, info.LastWriteTimeUtc.ToFileTimeUtc());
    }
}
