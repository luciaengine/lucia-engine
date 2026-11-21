using System.IO.Compression;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using GameEngine.Scripting.Runtime;
using GameEngine.SourceGenerators;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace GameEngine.Scripting.Web;

/// <summary>Compiles a project's C# sources and runs its systems for a browser play session.</summary>
internal sealed class WebScriptSession : IDisposable
{
    private readonly MetadataReference[] m_References;
    private AssemblyLoadContext? m_Context;

    public WebScriptSession()
    {
        m_References = ReadReferences().Select(image => MetadataReference.CreateFromImage(image)).ToArray();
    }

    /// <summary>Returns compiler diagnostics when the project cannot start.</summary>
    public string? Start(ulong world, IReadOnlyDictionary<string, string> sources)
    {
        Dispose();
        if (sources.Count == 0)
            return null;

        var parseOptions = new CSharpParseOptions(LanguageVersion.Latest);
        var syntax = sources.Select(source => CSharpSyntaxTree.ParseText(source.Value, parseOptions, source.Key));
        var name = "GameEngine.WebProject." + Guid.NewGuid().ToString("N");
        var compilation = CSharpCompilation.Create(name, syntax, m_References,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));
        var generator = new EntitySystemGenerator().AsSourceGenerator();
        CSharpGeneratorDriver.Create(new[] { generator }, parseOptions: parseOptions)
            .RunGeneratorsAndUpdateCompilation(compilation, out var generated, out var generatorDiagnostics);

        using var image = new MemoryStream();
        var result = generated.Emit(image);
        if (!result.Success || generatorDiagnostics.Any(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error))
        {
            return string.Join(Environment.NewLine, generatorDiagnostics.Concat(result.Diagnostics)
                .Where(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error));
        }

        var context = new AssemblyLoadContext(name, isCollectible: true);
        try
        {
            image.Position = 0;
            var assembly = context.LoadFromStream(image);
            RuntimeHelpers.RunModuleConstructor(assembly.ManifestModule.ModuleHandle);
            GameSystemRunner.Initialize(world, assembly);
            m_Context = context;
            return null;
        }
        catch
        {
            GameSystemRunner.Shutdown();
            context.Unload();
            throw;
        }
    }

    public void Tick(float seconds)
    {
        if (m_Context != null)
            GameSystemRunner.Tick(seconds);
    }

    public void Dispose()
    {
        var context = m_Context;
        if (context == null)
            return;
        m_Context = null;
        try
        {
            GameSystemRunner.Shutdown();
        }
        finally
        {
            context.Unload();
        }
    }

    private static IEnumerable<byte[]> ReadReferences()
    {
        using var references = typeof(WebScriptSession).Assembly.GetManifestResourceStream("Lucia.ScriptReferences")
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
