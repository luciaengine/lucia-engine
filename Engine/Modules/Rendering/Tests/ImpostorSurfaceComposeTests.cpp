// The impostor card material composes and compiles: the impostor surface, the card vertex
// modifier and the cutout, on the instanced world pass and in the depth prepass. The packed
// forest card, the packed forest near meshes and the forest canopy surface of the overview mesh
// compose too.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <algorithm>
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

MaterialBuildResult Build(const MaterialDocument& document, MaterialKeyword keywords)
{
    MaterialBuildContext context{};
    context.AdapterShaderDir = Tests::GetAdapterShaderDir();
    context.CacheRoot = TestUtils::MakeUniqueTempDirectory("ge_impostor_surface");
    auto result = BuildMaterialToShaderPackage(
        document, TestPaths::StagedEngineAssetsDir() / "Materials" / "ImpostorCardProbe.material",
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

void ExpectCompiles(const MaterialDocument& document, MaterialKeyword keywords, const char* label)
{
    const auto result = Build(document, keywords);
    if (ShadercMissing(result))
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& error : result.errors)
        ADD_FAILURE() << label << ": " << error;
    EXPECT_TRUE(result.success) << label;
}

} // namespace

TEST(ImpostorSurfaceCompose, TheCardComposesOnTheInstancedWorldPass)
{
    ExpectCompiles(ImpostorDocument(), MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows,
                   "world pass");
}

TEST(ImpostorSurfaceCompose, TheCardComposesInTheDepthPrepass)
{
    ExpectCompiles(ImpostorDocument(), MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment, "depth prepass");
}

TEST(ImpostorSurfaceCompose, ThePackedForestCardComposesForColorAndDepth)
{
    struct RestoreProfile
    {
        bool Previous = IsCompatShaderProfile();
        ~RestoreProfile() { SetCompatShaderProfile(Previous); }
    } restore;
    MaterialDocument card = ImpostorDocument();
    card.vertexModifier = "VertexModifiers/forest_impostor.glsl";
    card.customVertexShader = true;
    for (bool compatibility : {false, true})
    {
        SCOPED_TRACE(compatibility);
        SetCompatShaderProfile(compatibility);
        ExpectCompiles(card, MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows, "packed forest color");
        ExpectCompiles(card, MaterialKeyword::DepthOnlyFragment, "packed forest depth");
    }
}

TEST(ImpostorSurfaceCompose, TheForestCanopySurfaceComposesForColorAndDepth)
{
    MaterialDocument canopy{};
    canopy.materialName = "ForestCanopyProbe";
    canopy.lightingModel = "StandardPBR";
    canopy.surfaceShader = "Surfaces/forest_canopy.glsl";
    canopy.alphaMode = MaterialAlphaMode::Mask;
    canopy.doubleSided = true;
    canopy.properties["alphaCutoff"] = 0.25f;
    canopy.textures["albedoMap"] = std::string{};
    ExpectCompiles(canopy, MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows,
                   "canopy color");
    ExpectCompiles(canopy, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment, "canopy depth");
}

TEST(ImpostorSurfaceCompose, PackedNearMeshesReflectTheTransformBufferName)
{
    MaterialDocument mesh{};
    mesh.materialName = "ForestNearMeshProbe";
    mesh.lightingModel = "StandardPBR";
    mesh.surfaceShader = "Surfaces/standard_pbr.glsl";
    mesh.alphaMode = MaterialAlphaMode::Mask;
    mesh.doubleSided = true;
    const auto result = Build(mesh, MaterialKeyword::Instanced | MaterialKeyword::ForestMeshInstances |
                                        MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows);
    if (ShadercMissing(result))
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& error : result.errors)
        ADD_FAILURE() << error;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);

    const auto& sets = result.package->meta.Sets;
    const auto set = std::ranges::find(sets, 0u, &DescriptorSetMeta::Set);
    ASSERT_NE(set, sets.end());
    const auto binding = std::ranges::find(set->Bindings, 68u, &DescriptorBindingMeta::Binding);
    ASSERT_NE(binding, set->Bindings.end());
    EXPECT_EQ(binding->ResolvedNameId(), HashStringId("ForestMeshTransforms")) << binding->Name;
    EXPECT_EQ(binding->Type, ShaderMetaBindingType::kStorageBuffer);
}
