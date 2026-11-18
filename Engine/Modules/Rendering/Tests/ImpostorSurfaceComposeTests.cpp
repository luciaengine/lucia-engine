// The impostor card material composes and compiles: the impostor surface, the card vertex
// modifier and the cutout, on the instanced world pass and in the depth prepass.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

MaterialDocument ImpostorDocument()
{
    MaterialDocument doc{};
    doc.materialName = "ImpostorCardProbe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/impostor.glsl";
    doc.vertexModifier = "VertexModifiers/impostor_card.glsl";
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.doubleSided = true;
    return doc;
}

MaterialBuildResult Build(MaterialKeyword keywords)
{
    MaterialBuildContext context{};
    context.AdapterShaderDir = Tests::GetAdapterShaderDir();
    context.CacheRoot = TestUtils::MakeUniqueTempDirectory("ge_impostor_surface");
    auto result = BuildMaterialToShaderPackage(
        ImpostorDocument(), TestPaths::StagedEngineAssetsDir() / "Materials" / "ImpostorCardProbe.material",
        "ImpostorCardProbe", context, ShaderSourceKind::SpirV, keywords);
    std::error_code error;
    fs::remove_all(context.CacheRoot, error);
    return result;
}

bool ShadercMissing(const MaterialBuildResult& result)
{
    for (const auto& error : result.errors)
        if (error.find("shaderc is not available") != std::string::npos)
            return true;
    return false;
}

void ExpectCompiles(MaterialKeyword keywords, const char* label)
{
    const auto result = Build(keywords);
    if (ShadercMissing(result))
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& error : result.errors)
        ADD_FAILURE() << label << ": " << error;
    EXPECT_TRUE(result.success) << label;
}

} // namespace

TEST(ImpostorSurfaceCompose, TheCardComposesOnTheInstancedWorldPass)
{
    ExpectCompiles(MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows, "world pass");
}

TEST(ImpostorSurfaceCompose, TheCardComposesInTheDepthPrepass)
{
    ExpectCompiles(MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment, "depth prepass");
}
