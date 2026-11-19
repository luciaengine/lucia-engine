#include "Rendering/Core/Device.h"
#include "Rendering/Core/HotReloadIntegration.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
using namespace GameEngine::Rendering;

bool WriteColor(std::string_view source)
{
    std::ofstream stream("/Fixture/Color.glsl");
    stream << source;
    stream.close();
    NotifyShaderSourcesChanged();
    return stream.good();
}

std::string RunShaderCompilerTest()
{
    if (!ShaderCompileService::IsCompilerAvailable())
        return "Compiler unavailable.";
    std::filesystem::create_directories("/Fixture");
    ShaderProgramCompileRequest request{};
    request.debugName = "Browser compiler test";
    request.baseDirectory = "/Fixture";
    request.cacheRoot = "/Cache";
    request.stages.push_back({
        .stage = "fs",
        .sourcePath = "Color.frag",
        .inlineSource = "#version 450\n"
                        "#extension GL_GOOGLE_include_directive : require\n"
                        "#include \"Color.glsl\"\n"
                        "layout(location = 0) out vec4 outColor;\n"
                        "layout(set = 0, binding = 0) uniform texture2D shadowDepth;\n"
                        "layout(set = 0, binding = 1) uniform samplerShadow shadowCompare;\n"
                        "void main() {\n"
                        "    outColor = FixtureColor() * texture(sampler2DShadow(shadowDepth, shadowCompare), vec3(0.5));\n"
                        "}\n",
    });
    if (!WriteColor("vec4 FixtureColor() { return vec4(1, 0, 0, 1); }\n"))
        return "Could not write shader fixture.";

    const uint64_t initialCompiles = ShaderCompileService::GetTotalCompilations();
    std::string diagnostic;
    ShaderProgramCompileResult red{};
    if (!ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::Wgsl, red, &diagnostic))
        return diagnostic;
    if (red.meta.Sets.size() != 1 || red.meta.Sets.front().Set != 0)
        return "The shader package is missing its shadow descriptor set.";
    const auto& bindings = red.meta.Sets.front().Bindings;
    const auto comparisonSampler = std::ranges::find(bindings, 1u, &DescriptorBindingMeta::Binding);
    if (comparisonSampler == bindings.end() || !comparisonSampler->Image || !comparisonSampler->Image->Depth)
        return "The standalone shadow sampler was not reflected as a comparison sampler.";

    ShaderProgramCompileResult cached{};
    if (!ShaderCompileService::ServeCachedProgram(red.shaderPkgPath, ShaderSourceKind::Wgsl, cached) ||
        cached.stageBytes != red.stageBytes || ShaderCompileService::GetTotalCompilations() != initialCompiles + 1)
        return "The shader package could not serve the compiled WGSL.";

    if (!WriteColor("vec4 FixtureColor() { return vec4(0, 1, 0, 1); }\n"))
        return "Could not edit shader fixture.";
    ShaderProgramCompileResult green{};
    if (!ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::Wgsl, green, &diagnostic))
        return diagnostic;
    if (green.shaderPkgPath == red.shaderPkgPath || green.stageBytes == red.stageBytes)
        return "Editing an include did not produce fresh WGSL.";

    if (!WriteColor("vec4 FixtureColor() { malformed shader; }\n"))
        return "Could not write malformed shader fixture.";
    ShaderProgramCompileResult invalid{};
    if (ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::Wgsl, invalid, &diagnostic) ||
        diagnostic.find("Color.glsl") == std::string::npos || std::filesystem::exists(invalid.shaderPkgPath))
        return "Malformed source did not produce a file diagnostic without caching.";
    return {};
}
} // namespace

int main()
{
    const std::string error = RunShaderCompilerTest();
    if (!error.empty())
        std::fprintf(stderr, "%s\n", error.c_str());
    EM_ASM({
        document.querySelector('#result').textContent = JSON.stringify({
            passed: $0 === 0, error: UTF8ToString($1)
        });
    }, error.empty() ? 0 : 1, error.c_str());
    return error.empty() ? 0 : 1;
}
