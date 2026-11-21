using System.Runtime.InteropServices.JavaScript;
using System.Runtime.Loader;
using System.Runtime.Versioning;

namespace Lucia.Web.Editor;

[SupportedOSPlatform("browser")]
internal static partial class Program
{
    // Native editor startup and frame scheduling run from main.mjs.
    public static void Main()
    {
    }

    [JSExport]
    public static Task LoadCompilerAssembly(string name, byte[] image)
    {
        try
        {
            var context = AssemblyLoadContext.Default;
            if (!context.Assemblies.Any(assembly => assembly.GetName().Name == name))
            {
                using var stream = new MemoryStream(image, writable: false);
                context.LoadFromStream(stream);
            }
            return Task.CompletedTask;
        }
        catch (Exception error)
        {
            return Task.FromException(error);
        }
    }
}
