using System.Runtime.InteropServices.JavaScript;
using System.Runtime.Versioning;
using System.Text.Json;

using GameEngine.Scripting.Web;

namespace Lucia.Web.Editor;

[SupportedOSPlatform("browser")]
public static partial class BrowserPlayMode
{
    // The browser host can load this type before the script compiler assembly.
    private static IDisposable? s_Session;

    [JSExport]
    public static Task<string?> Start(int worldAddress, string sourcesJson)
    {
        try
        {
            var sources = JsonSerializer.Deserialize<Dictionary<string, string>>(sourcesJson)
                ?? throw new InvalidDataException("Invalid project script sources.");
            var session = (WebScriptSession)(s_Session ??= new WebScriptSession());
            return Task.FromResult(session.Start(unchecked((uint)worldAddress), sources));
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
            (s_Session as WebScriptSession)?.Tick(seconds);
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
}
