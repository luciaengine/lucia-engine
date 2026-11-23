// The composed impostor bake variant: the forward adapter runs the material's own surface and
// alpha cutoff and writes base colour and coverage at location 0 and the normal and depth at
// location 1, instead of shading. Composed from the shipped tree materials: the bark on the
// standard surface and the cutout leaves on the package's own surface.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif

namespace GameEngine::GlslShim::ImpostorBakeNormal
{
#include "Shaders/Includes/impostor_bake_normal.glsl"
} // namespace GameEngine::GlslShim::ImpostorBakeNormal

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

constexpr MaterialKeyword kBakePass = MaterialKeyword::Instanced | MaterialKeyword::ImpostorBake;

MaterialBuildResult Build(const MaterialDocument& doc, MaterialKeyword keywords,
                          VertexAttributeFlags attributes = VertexAttributeFlags::None)
{
    MaterialBuildContext context{};
    context.AdapterShaderDir = Tests::GetAdapterShaderDir();
    context.PackageShaderDirs = {fs::path(EZTREE_PACKAGE_SHADERS_DIR)};
    context.CacheRoot = TestUtils::MakeUniqueTempDirectory("ge_impostor_bake_variant");
    auto result = BuildMaterialToShaderPackage(
        doc, TestPaths::StagedEngineAssetsDir() / "Materials" / "ImpostorBakeProbe.material",
        "ImpostorBakeProbe", context, ShaderSourceKind::SpirV, keywords, attributes);
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

bool HasWindSine(const MaterialBuildResult& result)
{
    constexpr size_t kSpirvHeaderWords = 5;
    constexpr uint32_t kOpExtInst = 12;
    constexpr uint32_t kGlslStd450Sin = 13;
    const auto& bytes = result.package->stageBytes.at("vs");
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
    for (size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t instruction = words[at];
        const size_t count = instruction >> 16u;
        if (count == 0 || count > words.size() - at)
            return false;
        if ((instruction & 0xFFFFu) == kOpExtInst && count >= 5 && words[at + 4] == kGlslStd450Sin)
            return true;
        at += count;
    }
    return false;
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


TEST(ImpostorBakeVariant, FoliageWindKeepsAuthoredLeafPivotsAndBakesTheRestPose)
{
    auto leaves = BarkDocument();
    leaves.alphaMode = MaterialAlphaMode::Mask;
    leaves.vertexModifier = "VertexModifiers/foliage_wind.glsl";
    leaves.properties["foliageLeafPivots"] = 1.0f;
    leaves.properties["foliageWindMotion"] = std::vector<float>{0.30f, 0.18f};
    constexpr auto leafAttributes = VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasTangent |
                                    VertexAttributeFlags::HasUV6 | VertexAttributeFlags::HasUV7;
    const auto bake = Build(leaves, kBakePass, leafAttributes);
    if (ShadercMissing(bake))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectTheTwoBakeTargets(bake, "foliage bake");
    ASSERT_TRUE(bake.success);
    EXPECT_FALSE(HasWindSine(bake));

    for (const auto pass : {MaterialKeyword::None, MaterialKeyword::DepthOnlyFragment,
                            MaterialKeyword::MotionVectors})
    {
        SCOPED_TRACE(static_cast<uint64_t>(pass));
        const auto live = Build(leaves, MaterialKeyword::Instanced | pass, leafAttributes);
        ASSERT_TRUE(live.success) << (live.errors.empty() ? "" : live.errors.front());
        EXPECT_TRUE(HasWindSine(live));
    }

    auto bark = BarkDocument();
    bark.vertexModifier = leaves.vertexModifier;
    bark.properties["foliageWindMotion"] = std::vector<float>{0.30f, 0.0f};
    const auto sway = Build(bark, MaterialKeyword::Instanced, VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(sway.success) << (sway.errors.empty() ? "" : sway.errors.front());
    EXPECT_TRUE(HasWindSine(sway));
}

TEST(ImpostorBakeVariant, ATwoSidedLeafBakesWithTheTwoSidedNormalRule)
{
    auto leaves = LeafDocument();
    leaves.doubleSided = true;
    const auto twoSided = Build(leaves, kBakePass);
    if (ShadercMissing(twoSided))
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(twoSided.success) << (twoSided.errors.empty() ? "" : twoSided.errors.front());
    const auto& defines = twoSided.composedDefines;
    EXPECT_NE(std::find(defines.begin(), defines.end(), "GE_TWO_SIDED"), defines.end());

    const auto bark = Build(BarkDocument(), kBakePass);
    ASSERT_TRUE(bark.success) << (bark.errors.empty() ? "" : bark.errors.front());
    EXPECT_EQ(std::find(bark.composedDefines.begin(), bark.composedDefines.end(), "GE_TWO_SIDED"),
              bark.composedDefines.end())
        << "a one-sided surface keeps its own normal";
}

TEST(ImpostorBakeVariant, AnEdgeOnLeafKeepsItsUpperSideInAHorizonFrame)
{
    using GameEngine::GlslShim::vec3;
    using GameEngine::GlslShim::ImpostorBakeNormal::GE_ImpostorTwoSidedBakeNormal;
    // A horizon frame looks along -X. A level leaf is edge-on to it: either side may face the frame
    // camera by a hair, and both sides bake facing up.
    const vec3 towardHorizonFrame(1.0f, 0.0f, 0.0f);
    for (const vec3 leaf : {vec3(0.05f, -0.998f, 0.0f), vec3(-0.05f, 0.998f, 0.0f), vec3(0.0f, -1.0f, 0.0f)})
        EXPECT_GT(GE_ImpostorTwoSidedBakeNormal(leaf, towardHorizonFrame).y, 0.0f);

    // A leaf that faces the frame camera keeps facing it; one that faces away turns toward it.
    EXPECT_GT(GE_ImpostorTwoSidedBakeNormal(vec3(1.0f, 0.0f, 0.0f), towardHorizonFrame).x, 0.0f);
    EXPECT_GT(GE_ImpostorTwoSidedBakeNormal(vec3(-1.0f, 0.0f, 0.0f), towardHorizonFrame).x, 0.0f);

    // From above, the rule is the plain two-sided flip toward the camera.
    const vec3 fromAbove(0.0f, 1.0f, 0.0f);
    EXPECT_GT(GE_ImpostorTwoSidedBakeNormal(vec3(0.0f, -1.0f, 0.0f), fromAbove).y, 0.0f);
}
