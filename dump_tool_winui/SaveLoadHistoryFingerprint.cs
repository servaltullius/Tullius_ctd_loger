using System.Globalization;
using System.Text;
using System.Text.Json;

namespace SkyrimDiagDumpToolWinUI;

// ADR-0025: the save-load rows of crash_history.json a damaged-save judgement
// read, computed the way the analyzer does (CrashHistory::
// SaveLoadHistoryFingerprint). A stored summary whose fingerprint differs was
// judged on another history: an earlier failed load analyzed afterwards, or
// a row that was removed. The viewer analyzes such a dump again.
internal static class SaveLoadHistoryFingerprint
{
    private const ulong FnvOffset = 14695981039346656037ul;
    private const ulong FnvPrime = 1099511628211ul;

    // The fingerprint of the history at historyPath without the rows of the
    // dump whose content is excludeSha256; null when the file cannot be read
    // as a crash history (the caller treats that as changed). A missing file
    // is an empty history.
    public static string? Compute(string historyPath, string excludeSha256)
    {
        if (!File.Exists(historyPath))
        {
            return Format(0);
        }

        try
        {
            using var stream = new FileStream(historyPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var document = JsonDocument.Parse(stream);
            return ComputeFrom(document.RootElement, excludeSha256);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    public static string? ComputeFrom(JsonElement root, string excludeSha256)
    {
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("entries", out var entries) ||
            entries.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        ulong sum = 0;
        foreach (var entry in entries.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.Object)
            {
                continue;
            }
            var saveKey = ReadString(entry, "loading_save_key");
            if (saveKey.Length == 0)
            {
                continue;
            }
            var identity = ReadString(entry, "dump_identity_key");
            var dot = identity.IndexOf('.');
            var content = dot < 0 ? identity : identity[..dot];
            if (excludeSha256.Length != 0 && content == excludeSha256)
            {
                continue;
            }
            // The analyzer drops placeholders older writers stored
            // (UpsertHistoryEntry).
            var place = ReadString(entry, "loading_fault_place");
            if (place.EndsWith("|||", StringComparison.Ordinal) || place == "freeze|")
            {
                place = string.Empty;
            }
            var captureUnix = entry.TryGetProperty("capture_unix", out var captured) &&
                              captured.ValueKind == JsonValueKind.Number &&
                              captured.TryGetUInt64(out var capturedValue)
                ? capturedValue
                : 0ul;
            var row = string.Join(
                '\u001f',
                identity,
                saveKey,
                place,
                ReadString(entry, "loading_attempt_key"),
                captureUnix.ToString(CultureInfo.InvariantCulture));
            unchecked
            {
                sum += Fnv1a64(Encoding.UTF8.GetBytes(row));
            }
        }
        return Format(sum);
    }

    private static ulong Fnv1a64(byte[] bytes)
    {
        var hash = FnvOffset;
        foreach (var b in bytes)
        {
            unchecked
            {
                hash = (hash ^ b) * FnvPrime;
            }
        }
        return hash;
    }

    private static string Format(ulong value) => value.ToString("x16", CultureInfo.InvariantCulture);

    private static string ReadString(JsonElement node, string name) =>
        node.TryGetProperty(name, out var child) && child.ValueKind == JsonValueKind.String
            ? child.GetString() ?? string.Empty
            : string.Empty;
}
