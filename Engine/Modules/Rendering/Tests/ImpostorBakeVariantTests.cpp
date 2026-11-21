#include "Rendering/Materials/Shader/ShaderProfileDefines.h"
// The composed impostor bake variant: the forward adapter runs the material's own surface and
// alpha cutoff and writes base colour and coverage at location 0 and the normal and depth at
// location 1 and the season mask at location 2. Composed from the shipped tree materials: the bark on the
// standard surface and the cutout leaves on the package's own surface.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif

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
        doc, context.CacheRoot / "ImpostorBakeProbe.material",
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

template <typename Match>
bool HasSpirvInstruction(const MaterialBuildResult& result, const char* stage, Match match)
{
    constexpr size_t kSpirvHeaderWords = 5;
    const auto& bytes = result.package->stageBytes.at(stage);
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
    for (size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t instruction = words[at];
        const size_t count = instruction >> 16u;
        if (count == 0 || count > words.size() - at)
            return false;
        if (match(instruction & 0xFFFFu, std::span(words).subspan(at + 1, count - 1)))
            return true;
        at += count;
    }
    return false;
}

bool HasExplicitLodSample(const MaterialBuildResult& result)
{
    constexpr uint32_t kOpImageSampleExplicitLod = 88;
    return HasSpirvInstruction(result, "fs",
                               [](uint32_t opcode, auto) { return opcode == kOpImageSampleExplicitLod; });
}

bool HasWindSine(const MaterialBuildResult& result)
{
    constexpr uint32_t kOpExtInst = 12;
    constexpr uint32_t kGlslStd450Sin = 13;
    return HasSpirvInstruction(result, "vs", [](uint32_t opcode, std::span<const uint32_t> operands)
    {
        return opcode == kOpExtInst && operands.size() >= 4 && operands[3] == kGlslStd450Sin;
    });
}

void ExpectBakeTargets(const MaterialBuildResult& result, const char* label)
{
    for (const auto& error : result.errors)
        ADD_FAILURE() << label << ": " << error;
    ASSERT_TRUE(result.success) << label;
    const StageMeta* fragment = FragmentStage(result);
    ASSERT_NE(fragment, nullptr) << label << ": no fragment stage";
    const std::array<std::string_view, 3> expectedOutputs{"oBakeColor", "oBakeNormalDepth", "oBakeSeasonMask"};
    ASSERT_EQ(fragment->Outputs.size(), expectedOutputs.size()) << label;
    for (const auto& output : fragment->Outputs)
    {
        ASSERT_LT(output.Location, expectedOutputs.size()) << label;
        EXPECT_EQ(output.Name, expectedOutputs[output.Location]) << label;
    }
}

} // namespace

TEST(ImpostorBakeVariant, BarkBakeWritesColourThenNormalAndDepth)
{
    const auto bark = Build(BarkDocument(), kBakePass);
    if (ShadercMissing(bark))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectBakeTargets(bark, "bark bake");
}

TEST(ImpostorBakeVariant, CutoutLeafBakeComposesWithItsOwnSurface)
{
    const auto leaves = Build(LeafDocument(), kBakePass);
    if (ShadercMissing(leaves))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectBakeTargets(leaves, "leaf bake");
}

TEST(ImpostorBakeVariant, TransmissionAtlasHasAnOutputForBothBarkAndLeaves)
{
    const auto pass = kBakePass | MaterialKeyword::ImpostorBakeTransmission;
    const auto bark = Build(BarkDocument(), pass);
    if (ShadercMissing(bark))
        GTEST_SKIP() << "shaderc not built into this target";
    MaterialDocument leaves = BarkDocument();
    leaves.alphaMode = MaterialAlphaMode::Mask;
    const auto leaf = Build(leaves, pass | MaterialKeyword::DiffuseTransmission);
    for (const auto* result : {&bark, &leaf})
    {
        ASSERT_TRUE(result->success);
        const StageMeta* fragment = FragmentStage(*result);
        ASSERT_NE(fragment, nullptr);
        ASSERT_EQ(fragment->Outputs.size(), 4u);
        bool foundTransmission = false;
        for (const auto& output : fragment->Outputs)
            foundTransmission |= output.Location == 3u && output.Name == "oBakeDiffuseTransmission";
        EXPECT_TRUE(foundTransmission);
    }

    MaterialDocument card = BarkDocument();
    card.surfaceShader = "Surfaces/impostor.glsl";
    card.alphaMode = MaterialAlphaMode::Mask;
    const auto live = Build(card, MaterialKeyword::Instanced | MaterialKeyword::DiffuseTransmission);
    EXPECT_TRUE(live.success);
}

TEST(ImpostorBakeVariant, StockMaskedAlbedoUsesAnExplicitMipOnlyInTheBake)
{
    MaterialDocument leaves = BarkDocument();
    leaves.materialName = "ImpostorBakeStockLeaves";
    leaves.alphaMode = MaterialAlphaMode::Mask;
    const auto bakedLeaves = Build(leaves, kBakePass);
    if (ShadercMissing(bakedLeaves))
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(bakedLeaves.success);
    EXPECT_TRUE(HasExplicitLodSample(bakedLeaves));

    const auto bakedBark = Build(BarkDocument(), kBakePass);
    ASSERT_TRUE(bakedBark.success);
    EXPECT_FALSE(HasExplicitLodSample(bakedBark));

    const auto meshLeaves = Build(leaves, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment);
    ASSERT_TRUE(meshLeaves.success);
    EXPECT_FALSE(HasExplicitLodSample(meshLeaves));
}

TEST(ImpostorBakeVariant, AnimatedMaterialBakesItsRestPose)
{
    MaterialDocument leaves = LeafDocument();
    leaves.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    const auto bake = Build(leaves, kBakePass);
    if (ShadercMissing(bake))
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(bake.success);
    EXPECT_FALSE(HasWindSine(bake));

    const auto live = Build(leaves, MaterialKeyword::Instanced);
    ASSERT_TRUE(live.success);
    EXPECT_TRUE(HasWindSine(live));
}

TEST(ImpostorBakeVariant, FoliageSwayCompilesForMeshAndCardMotion)
{
    MaterialDocument mesh = BarkDocument();
    mesh.vertexModifier = "VertexModifiers/foliage_wind.glsl";
    constexpr MaterialKeyword motion = MaterialKeyword::Instanced | MaterialKeyword::MotionVectors;
    const auto meshMotion = Build(mesh, motion);
    if (ShadercMissing(meshMotion))
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(meshMotion.success);
    EXPECT_TRUE(HasWindSine(meshMotion));

    MaterialDocument card = BarkDocument();
    card.surfaceShader = "Surfaces/impostor.glsl";
    card.vertexModifier = "VertexModifiers/impostor_card.glsl";
    card.alphaMode = MaterialAlphaMode::Mask;
    const auto cardMotion = Build(card, motion);
    ASSERT_TRUE(cardMotion.success);
    EXPECT_TRUE(HasWindSine(cardMotion));
}

TEST(ImpostorBakeVariant, AuthoredLeafPivotsCompileForLivePassesAndBakeInTheRestPose)
{
    MaterialDocument leaves = BarkDocument();
    leaves.alphaMode = MaterialAlphaMode::Mask;
    leaves.vertexModifier = "VertexModifiers/foliage_wind.glsl";
    leaves.properties["foliageLeafPivots"] = 1.0f;
    constexpr auto attributes = VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasUV6 |
                                VertexAttributeFlags::HasUV7;
    const auto bake = Build(leaves, kBakePass, attributes);
    if (ShadercMissing(bake))
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(bake.success);
    EXPECT_FALSE(HasWindSine(bake));
    for (const MaterialKeyword pass : {MaterialKeyword::None, MaterialKeyword::DepthOnlyFragment,
                                       MaterialKeyword::MotionVectors})
    {
        SCOPED_TRACE(static_cast<uint64_t>(pass));
        const auto live = Build(leaves, MaterialKeyword::Instanced | pass, attributes);
        ASSERT_TRUE(live.success) << (live.errors.empty() ? "" : live.errors.front());
        EXPECT_TRUE(HasWindSine(live));
    }
}

TEST(ImpostorBakeVariant, TheBakeShapeDropsTheReflectionGBuffer)
{
    // The world pass ORs SSSR onto every key of a view with reflections; a bake key that carries it
    // must compose the same two targets, not the shading path's G-buffer slices.
    const auto bark = Build(BarkDocument(), kBakePass | MaterialKeyword::SSSRNormalRoughness);
    if (ShadercMissing(bark))
        GTEST_SKIP() << "shaderc not built into this target";
    ExpectBakeTargets(bark, "bark bake with SSSR");
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

TEST(ImpostorBakeVariant, CompatibilityProfileComposesAtlasBakeColorAndDepth)
{
    struct RestoreCompatProfile
    {
        bool Previous = IsCompatShaderProfile();
        ~RestoreCompatProfile() { SetCompatShaderProfile(Previous); }
    } restore;
    SetCompatShaderProfile(true);

    auto leaves = LeafDocument();
    const auto bake = Build(leaves, MaterialKeyword::ImpostorBake | MaterialKeyword::ImpostorBakeTransmission);
    if (ShadercMissing(bake))
        GTEST_SKIP() << "shaderc is not available";
    ASSERT_TRUE(bake.success) << ::testing::PrintToString(bake.errors);
    ASSERT_NE(FragmentStage(bake), nullptr);

    MaterialDocument card = BarkDocument();
    card.alphaMode = MaterialAlphaMode::Mask;
    card.surfaceShader = "Surfaces/impostor.glsl";
    card.vertexModifier = "VertexModifiers/impostor_card.glsl";
    card.properties["alphaCutoff"] = 0.5f;
    card.textures["albedoMap"] = std::string{};
    card.textures["normalMap"] = std::string{};
    card.textures["impostorSeasonMaskMap"] = std::string{};
    card.textures["impostorDiffuseTransmissionMap"] = std::string{};
    const auto color = Build(card, MaterialKeyword::Instanced | MaterialKeyword::DiffuseTransmission);
    ASSERT_TRUE(color.success) << ::testing::PrintToString(color.errors);
    const auto depth = Build(card, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment);
    ASSERT_TRUE(depth.success) << ::testing::PrintToString(depth.errors);
}
