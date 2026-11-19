using System.IO.Compression;
using System.Runtime.InteropServices.JavaScript;
using System.Runtime.Versioning;
using System.Text.Json;

using GameEngine.Scripting.Web;

namespace Lucia.Web.Editor;

[SupportedOSPlatform("browser")]
public static partial class BrowserPlayMode
{
    private static WebScriptSession? s_Session;

    [JSExport]
    public static Task<string?> Start(int worldAddress, string sourcesJson)
    {
        try
        {
            var sources = JsonSerializer.Deserialize<Dictionary<string, string>>(sourcesJson)
                ?? throw new InvalidDataException("Invalid project script sources.");
            s_Session ??= new WebScriptSession(ReadReferences());
            return Task.FromResult(s_Session.Start(unchecked((uint)worldAddress), sources));
        }
        catch (Exception error)
        {
            return Task.FromException<string?>(error);
        }
    }

    [JSExport]
    public static Task Tick(float seconds)
    {
        try
        {
            s_Session?.Tick(seconds);
            return Task.CompletedTask;
        }
        catch (Exception error)
        {
            return Task.FromException(error);
        }
    }

    [JSExport]
    public static Task Stop()
    {
        try
        {
            s_Session?.Dispose();
            return Task.CompletedTask;
        }
        catch (Exception error)
        {
            return Task.FromException(error);
        }
    }

    private static IEnumerable<byte[]> ReadReferences()
    {
        using var references = typeof(BrowserPlayMode).Assembly.GetManifestResourceStream("Lucia.ScriptReferences")
            ?? throw new InvalidOperationException("The browser script references are missing.");
        using var archive = new ZipArchive(references);
        foreach (var entry in archive.Entries)
        {
            using var stream = entry.Open();
            using var image = new MemoryStream();
            stream.CopyTo(image);
            yield return image.ToArray();
        }
    }
}
