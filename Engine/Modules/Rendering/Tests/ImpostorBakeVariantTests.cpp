// The composed impostor bake variant: the forward adapter runs the material's own surface and
// alpha cutoff and writes base colour and coverage at location 0 and the normal and depth at
// location 1, instead of shading. Composed from the shipped tree materials: the bark on the
// standard surface and the cutout leaves on the package's own surface.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <filesystem>
#include <string>

#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

constexpr MaterialKeyword kBakePass = MaterialKeyword::Instanced | MaterialKeyword::ImpostorBake;

MaterialBuildResult Build(const MaterialDocument& doc, MaterialKeyword keywords)
{
    MaterialBuildContext context{};
    context.AdapterShaderDir = Tests::GetAdapterShaderDir();
    context.PackageShaderDirs = {fs::path(EZTREE_PACKAGE_SHADERS_DIR)};
    context.CacheRoot = TestUtils::MakeUniqueTempDirectory("ge_impostor_bake_variant");
    auto result = BuildMaterialToShaderPackage(
        doc, TestPaths::StagedEngineAssetsDir() / "Materials" / "ImpostorBakeProbe.material",
        "ImpostorBakeProbe", context, ShaderSourceKind::SpirV, keywords);
    std::error_code error;
    fs::remove_all(context.CacheRoot, error);
    return result;
}

// The static twin of the generated bark: the bake draws the rest pose, without the wind modifier.
MaterialDocument BarkDocument()
{
    MaterialDocument doc{};
    doc.materialName = "ImpostorBakeBark";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    return doc;
}

// The static twin of the generated leaves: the package's leaf surface, alpha cutout.
MaterialDocument LeafDocument()
{
    MaterialDocument doc{};
    doc.materialName = "ImpostorBakeLeaves";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/ez_tree_leaves.glsl";
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.properties["alphaCutoff"] = 0.5f;
    return doc;
}

bool ShadercMissing(const MaterialBuildResult& result)
{
    for (const auto& error : result.errors)
        if (error.find("shaderc is not available") != std::string::npos)
            return true;
    return false;
}

const StageMeta* FragmentStage(const MaterialBuildResult& result)
{
    if (result.package == nullptr)
        return nullptr;
    const auto it = result.package->meta.Stages.find("fs");
    return it == result.package->meta.Stages.end() ? nullptr : &it->second;
}

void ExpectTheTwoBakeTargets(const MaterialBuildResult& result, const char* label)
{
    for (const auto& error : result.errors)
        ADD_FAILURE() << label << ": " << error;
    ASSERT_TRUE(result.success) << label;
    const StageMeta* fragment = FragmentStage(result);
    ASSERT_NE(fragment, nullptr) << label << ": no fragment stage";
    ASSERT_EQ(fragment->Outputs.size(), 2u) << label << ": the bake attaches exactly two colour targets";
    for (const auto& output : fragment->Outputs)
    {
        if (output.Location == 0u)
            EXPECT_EQ(output.Name, "oBakeColor") << label;
        else
            EXPECT_EQ(output.Name, "oBakeNormalDepth") << label;
    }
}

} // namespace

TEST(ImpostorBakeVariant, BarkBakeWritesColourThenNormalAndDepth)
{
    const auto bark = Build(BarkDocument(), kBakePass);
    if (ShadercMissing(bark))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectTheTwoBakeTargets(bark, "bark bake");
}

TEST(ImpostorBakeVariant, CutoutLeafBakeComposesWithItsOwnSurface)
{
    const auto leaves = Build(LeafDocument(), kBakePass);
    if (ShadercMissing(leaves))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectTheTwoBakeTargets(leaves, "leaf bake");
}

TEST(ImpostorBakeVariant, TheBakeShapeDropsTheReflectionGBuffer)
{
    // The world pass ORs SSSR onto every key of a view with reflections; a bake key that carries it
    // must compose the same two targets, not the shading path's G-buffer slices.
    const auto bark = Build(BarkDocument(), kBakePass | MaterialKeyword::SSSRNormalRoughness);
    if (ShadercMissing(bark))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectTheTwoBakeTargets(bark, "bark bake with SSSR");
}

TEST(ImpostorBakeVariant, TheAdapterRefusesTheBakeWithTheDepthOnlyShape)
{
    const auto refused = Build(BarkDocument(), kBakePass | MaterialKeyword::DepthOnlyFragment);
    if (ShadercMissing(refused))
        GTEST_SKIP() << "shaderc not built into this target";
    EXPECT_FALSE(refused.success);
    bool named = false;
    for (const auto& error : refused.errors)
        named = named || error.find("GE_IMPOSTOR_BAKE and GE_DEPTH_ONLY_FRAGMENT") != std::string::npos;
    EXPECT_TRUE(named) << "the refusal names the two shapes";
}
