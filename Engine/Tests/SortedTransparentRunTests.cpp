#include <gtest/gtest.h>

#include "Engine/Rendering/SortedTransparentRuns.h"
#include "Engine/Rendering/MaterialColorClassify.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{
Material MakeRunMaterial(const char* name, const char* surface,
                         const std::optional<MaterialBlendState>& blend,
                         uint64_t userKeywordHash = 0, bool customVertexShader = false)
{
    Material mat = Material::TestFactory::Create(GUID{}, name, 256);
    Material::TestFactory::SetAlphaMode(mat, MaterialAlphaMode::Blend);
    Material::TestFactory::SetBlendState(mat, blend);
    MaterialCompileSpec spec{};
    spec.surfaceShaderPath = surface;
    spec.lightingModel = "standardpbr";
    spec.customVertexShader = customVertexShader;
    Material::TestFactory::SetCompileSpec(mat, spec);
    Rendering::ShaderVariantKey vk{};
    vk.materialKeywords = Rendering::MaterialKeyword::AlphaBlend;
    vk.userKeywordHash = userKeywordHash;
    Material::TestFactory::SetVariantKey(mat, vk);
    return mat;
}
} // namespace

TEST(SortedTransparentRuns, CompatibilityKeepsDistinctMaterialBindingsInSeparateRuns)
{
    const Material a = MakeRunMaterial("atlasA", "standard_pbr.surface", std::nullopt);
    const Material b = MakeRunMaterial("atlasB", "standard_pbr.surface", std::nullopt);
    EXPECT_EQ(MakeSortedTransparentRunMaterialKey(a), MakeSortedTransparentRunMaterialKey(b));
    EXPECT_NE(MakeSortedTransparentRunMaterialKey(a, true), MakeSortedTransparentRunMaterialKey(b, true));
    EXPECT_EQ(MakeSortedTransparentRunMaterialKey(a, true), MakeSortedTransparentRunMaterialKey(a, true));
}

TEST(SortedTransparentRuns, MaterialKeySharesAcrossParamOnlyVariance)
{
    // Two glass materials that differ only in params/textures (which ride the
    // bindless materialIndex fetch) — SAME key, one run, one bind.
    const Material a = MakeRunMaterial("glassA", "standard_pbr.surface", std::nullopt);
    const Material b = MakeRunMaterial("glassB", "standard_pbr.surface", std::nullopt);
    EXPECT_TRUE(MakeSortedTransparentRunMaterialKey(a) == MakeSortedTransparentRunMaterialKey(b));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnBlendEquation)
{
    MaterialBlendState premult{};
    premult.SrcColorFactor = MaterialBlendFactor::One;
    premult.DstColorFactor = MaterialBlendFactor::OneMinusSrcAlpha;
    const Material straight = MakeRunMaterial("straight", "standard_pbr.surface", std::nullopt);
    const Material hwPremult = MakeRunMaterial("premult", "standard_pbr.surface", premult);
    // Hardware premultiplied is a distinct blend equation -> its own PSO ->
    // its own run (the design's premultiplied caveat, F1).
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(straight) ==
                 MakeSortedTransparentRunMaterialKey(hwPremult));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnSurfaceProgramAndUserKeywords)
{
    const Material pbr = MakeRunMaterial("pbr", "standard_pbr.surface", std::nullopt);
    const Material water = MakeRunMaterial("water", "water_stylized.surface", std::nullopt);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(pbr) ==
                 MakeSortedTransparentRunMaterialKey(water));

    // User keywords compile different SPIR-V; ColorClassSignature never needed
    // this lane (Blend is not color-merge eligible) but the run key must.
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt, 0);
    const Material keyworded = MakeRunMaterial("kw", "standard_pbr.surface", std::nullopt, 0xABCDEF12u);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                 MakeSortedTransparentRunMaterialKey(keyworded));
}

TEST(SortedTransparentRuns, MaterialKeySplitsOnCustomVertexShader)
{
    // customVertexShader changes the compiled variant AND clamps vertexFlags
    // to None at compile time, so the geometry key cannot catch the
    // difference — the material key must split it or two Blend materials
    // differing only here would fuse into one run and the representative's
    // pipeline would draw the other's records (R2-3).
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt);
    const Material procedural = MakeRunMaterial("proc", "standard_pbr.surface", std::nullopt,
                                                /*userKeywordHash=*/0,
                                                /*customVertexShader=*/true);
    EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                 MakeSortedTransparentRunMaterialKey(procedural));
}

TEST(SortedTransparentRuns, MaterialKeyAndColorClassSplitOnEveryKeywordBit)
{
    // Both keys that put several materials on one pipeline, this run key and
    // the opaque colour-class signature, carry the whole 64-bit keyword field:
    // two materials that differ in one keyword compile different programs, so
    // a key that dropped the bit would draw one through the other's pipeline.
    constexpr uint32_t kKeywordBits = 64;
    const Material plain = MakeRunMaterial("plain", "standard_pbr.surface", std::nullopt);
    for (uint32_t bit = 0; bit < kKeywordBits; ++bit)
    {
        const auto keyword = static_cast<Rendering::MaterialKeyword>(uint64_t{1} << bit);
        if (keyword == Rendering::MaterialKeyword::AlphaBlend)
            continue; // every run material carries it
        Material keyed = MakeRunMaterial("keyed", "standard_pbr.surface", std::nullopt);
        Rendering::ShaderVariantKey variantKey = keyed.GetVariantKey();
        variantKey.materialKeywords |= keyword;
        Material::TestFactory::SetVariantKey(keyed, variantKey);
        EXPECT_FALSE(MakeSortedTransparentRunMaterialKey(plain) ==
                     MakeSortedTransparentRunMaterialKey(keyed))
            << "bit " << bit;
        EXPECT_FALSE(ComputeColorClassSignature(plain) == ComputeColorClassSignature(keyed))
            << "bit " << bit;
    }
}
