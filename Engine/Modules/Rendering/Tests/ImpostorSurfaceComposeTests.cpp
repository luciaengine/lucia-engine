// The impostor card material composes and compiles: the impostor surface, the card vertex
// modifier and the cutout, on the instanced world pass and in the depth prepass. The packed
// forest card, the packed forest splats, the packed forest near meshes and the forest canopy surface
// of the overview mesh compose too.

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
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <spirv/unified1/spirv.hpp>

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

MaterialBuildResult Build(const MaterialDocument& document, MaterialKeyword keywords,
                          const ShaderCompileTarget& target = CurrentShaderCompileTarget())
{
    MaterialBuildContext context{};
    context.AdapterShaderDir = Tests::GetAdapterShaderDir();
    context.CacheRoot = TestUtils::MakeUniqueTempDirectory("ge_impostor_surface");
    auto result = BuildMaterialToShaderPackage(
        document, TestPaths::StagedEngineAssetsDir() / "Materials" / "ImpostorCardProbe.material",
        "ImpostorCardProbe", context, ShaderSourceKind::SpirV, keywords, VertexAttributeFlags::None, target);
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

TEST(ImpostorSurfaceCompose, TheForestCardFacesADirectionalViewer)
{
    MaterialDocument card = ImpostorDocument();
    card.vertexModifier = "VertexModifiers/forest_impostor.glsl";
    card.customVertexShader = true;
    const auto result = Build(card, MaterialKeyword::DepthOnlyFragment);
    if (ShadercMissing(result))
        GTEST_SKIP() << "shaderc not built into this target";
    for (const auto& error : result.errors)
        ADD_FAILURE() << error;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);

    // forestCard (set 2, binding 4) ends with the viewer: a point (w = 1) or a direction (w = 0).
    const auto& sets = result.package->meta.Sets;
    const auto set = std::ranges::find(sets, 2u, &DescriptorSetMeta::Set);
    ASSERT_NE(set, sets.end());
    const auto binding = std::ranges::find(set->Bindings, 4u, &DescriptorBindingMeta::Binding);
    ASSERT_NE(binding, set->Bindings.end());
    ASSERT_TRUE(binding->Block);
    EXPECT_EQ(binding->Block->Size, 64u);
    const auto& members = binding->Block->Members;
    const auto viewer = std::ranges::find(members, std::string("viewer"), &Member::Name);
    ASSERT_NE(viewer, members.end());
    EXPECT_EQ(viewer->Offset, 48u);
}

TEST(ImpostorSurfaceCompose, ThePackedForestSplatsComposeWithTheHandoffCrossfade)
{
    struct RestoreProfile
    {
        bool Previous = IsCompatShaderProfile();
        ~RestoreProfile() { SetCompatShaderProfile(Previous); }
    } restore;
    MaterialDocument splats{};
    splats.materialName = "ForestSplatProbe";
    splats.lightingModel = "StandardPBR";
    splats.surfaceShader = "Surfaces/forest_splat.glsl";
    splats.vertexModifier = "VertexModifiers/forest_splats.glsl";
    splats.customVertexShader = true;
    splats.alphaMode = MaterialAlphaMode::Mask;
    splats.doubleSided = true;
    splats.properties["alphaCutoff"] = 0.5f;
    for (bool compatibility : {false, true})
    {
        SCOPED_TRACE(compatibility);
        SetCompatShaderProfile(compatibility);
        ExpectCompiles(splats, MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::LodCrossfade,
                       "packed forest splats");
    }
}

// The adapter also discards in main. Inspect EvaluateSurface itself and require that demoted lanes
// rejoin the block that proceeds to the crown's implicit texture samples.
TEST(ImpostorSurfaceCompose, PackedCardRejoinsCrownSamplingAfterDemoteOnlyOnSupportedTargets)
{
    auto card = ImpostorDocument();
    card.vertexModifier = "VertexModifiers/forest_impostor.glsl";
    card.customVertexShader = true;
    for (const ShaderCompileTarget target : {
             ShaderCompileTarget{.Spirv = SpirvTarget::Spirv_1_6, .FragmentDemote = true},
             ShaderCompileTarget{.Spirv = SpirvTarget::Spirv_1_6},
             ShaderCompileTarget{.Spirv = SpirvTarget::Spirv_1_5, .FragmentDemote = true},
             ShaderCompileTarget{.Spirv = SpirvTarget::Spirv_1_6, .Compatibility = true, .FragmentDemote = true}})
    {
        SCOPED_TRACE(target.Hash());
        const auto result = Build(card, MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows, target);
        if (ShadercMissing(result))
            GTEST_SKIP() << "shaderc not built into this target";
        ASSERT_TRUE(result.success);
        const auto& module = result.package->stageBytes.at("fs");
        std::vector<uint32_t> words(module.size() / sizeof(uint32_t));
        ASSERT_EQ(words.size() * sizeof(uint32_t), module.size());
        std::memcpy(words.data(), module.data(), module.size());
        uint32_t surface = 0;
        uint32_t crown = 0;
        uint32_t function = 0;
        bool demoted = false;
        bool crownCalled = false;
        constexpr size_t kHeaderWords = 5;
        constexpr uint32_t kOpcodeMask = 0xffffu;
        constexpr uint32_t kWordCountShift = 16u;
        for (size_t offset = kHeaderWords; offset < words.size();)
        {
            const auto opcode = static_cast<spv::Op>(words[offset] & kOpcodeMask);
            const size_t wordCount = words[offset] >> kWordCountShift;
            ASSERT_GT(wordCount, 0u);
            ASSERT_LE(wordCount, words.size() - offset);
            if (opcode == spv::OpName)
            {
                ASSERT_GE(wordCount, 3u);
                const std::string_view name(reinterpret_cast<const char*>(&words[offset + 2]),
                                            (wordCount - 2) * sizeof(uint32_t));
                if (name.starts_with("EvaluateSurface("))
                    surface = words[offset + 1];
                if (name.starts_with("GE_ImpostorCrownLight("))
                    crown = words[offset + 1];
            }
            if (opcode == spv::OpFunction)
            {
                ASSERT_GE(wordCount, 3u);
                function = words[offset + 2];
            }
            if (surface != 0 && function == surface)
            {
                if (opcode == spv::OpDemoteToHelperInvocation)
                {
                    demoted = true;
                    const size_t branch = offset + wordCount;
                    ASSERT_LT(branch + 3, words.size());
                    EXPECT_EQ(words[branch] & kOpcodeMask, spv::OpBranch);
                    EXPECT_EQ(words[branch + 2] & kOpcodeMask, spv::OpLabel);
                    EXPECT_EQ(words[branch + 1], words[branch + 3]);
                }
                if (opcode == spv::OpFunctionCall)
                {
                    ASSERT_GE(wordCount, 4u);
                    if (words[offset + 3] == crown)
                    {
                        EXPECT_EQ(demoted, target.FragmentDemote && !target.Compatibility &&
                                               target.Spirv == SpirvTarget::Spirv_1_6);
                        crownCalled = true;
                    }
                }
            }
            offset += wordCount;
        }
        EXPECT_NE(surface, 0u);
        EXPECT_NE(crown, 0u);
        EXPECT_TRUE(crownCalled);
    }
}

TEST(ImpostorSurfaceCompose, PackedCardDemoteLeavesTheDepthProgramIdentical)
{
    auto card = ImpostorDocument();
    card.vertexModifier = "VertexModifiers/forest_impostor.glsl";
    card.customVertexShader = true;
    const ShaderCompileTarget kBaseline{.Spirv = SpirvTarget::Spirv_1_6};
    const ShaderCompileTarget kDemote{.Spirv = SpirvTarget::Spirv_1_6, .FragmentDemote = true};
    const auto baseline = Build(card, MaterialKeyword::DepthOnlyFragment, kBaseline);
    if (ShadercMissing(baseline))
        GTEST_SKIP() << "shaderc not built into this target";
    const auto candidate = Build(card, MaterialKeyword::DepthOnlyFragment, kDemote);
    for (const auto& error : baseline.errors)
        ADD_FAILURE() << error;
    for (const auto& error : candidate.errors)
        ADD_FAILURE() << error;
    ASSERT_TRUE(baseline.success);
    ASSERT_TRUE(candidate.success);
    EXPECT_EQ(baseline.package->stageBytes.at("fs"), candidate.package->stageBytes.at("fs"));
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
    // The overview quads of the splat handoff band draw without instancing, with the crossfade.
    ExpectCompiles(canopy, MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::LodCrossfade,
                   "canopy handoff");
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
