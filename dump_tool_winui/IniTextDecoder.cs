using System.Runtime.InteropServices;
using System.Text;

namespace SkyrimDiagDumpToolWinUI;

// SkyrimDiagHelper.ini decoded the way the helper decodes it
// (helper/include/SkyrimDiagHelper/IniText.h, DecodeIniBytes): a UTF-8 or
// UTF-16 BOM; UTF-16LE without one when the second byte is NUL; strict UTF-8;
// otherwise the ANSI code page. Read as UTF-8 only, an OutputDir with Korean
// letters saved in CP949 pointed the viewer at another folder than the one
// the helper writes to.
internal static class IniTextDecoder
{
    // `ansi`: the code page for bytes that are no UTF-8 (the system's when null).
    public static string Decode(byte[] bytes, Encoding? ansi = null)
    {
        if (bytes.Length == 0)
        {
            return string.Empty;
        }
        if (StartsWith(bytes, 0xEF, 0xBB, 0xBF))
        {
            return new UTF8Encoding(false, false).GetString(bytes, 3, bytes.Length - 3);
        }
        if (StartsWith(bytes, 0xFF, 0xFE))
        {
            return Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2);
        }
        if (StartsWith(bytes, 0xFE, 0xFF))
        {
            return Encoding.BigEndianUnicode.GetString(bytes, 2, bytes.Length - 2);
        }
        if (bytes.Length >= 2 && bytes.Length % 2 == 0 && bytes[0] != 0 && bytes[1] == 0)
        {
            return Encoding.Unicode.GetString(bytes);
        }
        try
        {
            return new UTF8Encoding(false, true).GetString(bytes);
        }
        catch (DecoderFallbackException)
        {
            return (ansi ?? AnsiEncoding()).GetString(bytes);
        }
    }

    // The system ANSI code page, as MultiByteToWideChar(CP_ACP) uses it.
    public static Encoding AnsiEncoding()
    {
        if (!OperatingSystem.IsWindows())
        {
            return Encoding.Latin1;
        }
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        try
        {
            return Encoding.GetEncoding((int)GetACP());
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException)
        {
            return Encoding.Latin1;
        }
    }

    private static bool StartsWith(byte[] bytes, params byte[] prefix)
    {
        if (bytes.Length < prefix.Length)
        {
            return false;
        }
        for (var i = 0; i < prefix.Length; i++)
        {
            if (bytes[i] != prefix[i])
            {
                return false;
            }
        }
        return true;
    }

    [DllImport("kernel32.dll")]
    private static extern uint GetACP();
}
