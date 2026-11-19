// The ocean surface is registered at runtime from a synthetic document, so no
// .material asset names Ocean/ocean_surface.glsl and no other suite composes it:
// an edit to the surface, its vertex modifier or the ocean_common.glsl parameter
// block they share reaches a compiler for the first time when a scene with water
// opens. This runs the shipped document and keywords through the real
// ShaderComposer -> ShaderCompileService path in both shader profiles, which are
// the two the cook and the runtime request.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Materials/Shader/ShaderProfileDefines.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "ScopedCompatShaderProfile.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

using TestSupport::ScopedCompatShaderProfile;

class OceanSurfaceCompose : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_EngineShaderDir = TestPaths::StagedRenderingShadersDir();
        if (!fs::exists(m_EngineShaderDir))
            GTEST_SKIP() << "Staged engine shader tree not found: " << m_EngineShaderDir.string();
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";

        m_CacheRoot = fs::temp_directory_path() / ("ge_ocean_compose_" + GUID::Generate().ToString());
        fs::create_directories(m_CacheRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_CacheRoot, ec);
    }

    // The registration pipeline, exactly as OceanForwardContributor and
    // MaterialVariantCook ask for it.
    MaterialBuildResult BuildOceanSurface(bool compatProfile)
    {
        MaterialBuildContext context{};
        context.AdapterShaderDir = m_EngineShaderDir;
        context.CacheRoot = m_CacheRoot / "Shaders";
        return BuildMaterialToShaderPackage(
            Ocean::BuildOceanSurfaceMaterialDocument(), m_CacheRoot / "ocean_surface.material",
            "ocean_surface", context, ShaderSourceKind::SpirV,
            Ocean::OceanSurfaceMaterialKeywords(compatProfile), VertexAttributeFlags::None);
    }

    fs::path m_EngineShaderDir;
    fs::path m_CacheRoot;
};
} // namespace

TEST_F(OceanSurfaceCompose, NativeProfileVariantCompiles)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}

TEST_F(OceanSurfaceCompose, CompatProfileVariantCompiles)
{
    ScopedCompatShaderProfile compat;
    const MaterialBuildResult built = BuildOceanSurface(true);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    EXPECT_FALSE(built.composedVertexSource.empty());
    EXPECT_FALSE(built.composedFragmentSource.empty());
}

// Vulkan counts the samplers (separate and combined) a stage can reach across a pipeline's sets
// against maxPerStageDescriptorSamplers, which is 16 on MoltenVK and in WebGPU. Set 1 is the bindless
// texture set, created update-after-bind, which the limit does not count.
TEST_F(OceanSurfaceCompose, NativeProfileVariantFitsTheSamplerLimitOfEveryStage)
{
    constexpr uint32_t kSamplersPerStage = 16;
    constexpr uint32_t kBindlessSet = 1;
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.package, nullptr);
    for (const uint32_t stageBit : {ShaderMetaStage::kVertex, ShaderMetaStage::kFragment})
    {
        uint32_t samplers = 0;
        std::string names;
        for (const DescriptorSetMeta& set : built.package->meta.Sets)
        {
            if (set.Set == kBindlessSet)
                continue;
            for (const DescriptorBindingMeta& binding : set.Bindings)
            {
                const bool sampler = binding.Type == ShaderMetaBindingType::kSampler ||
                                     binding.Type == ShaderMetaBindingType::kCombinedImageSampler;
                if (!sampler || (binding.StagesMask & stageBit) == 0u)
                    continue;
                samplers += binding.Count;
                names += binding.Name + " ";
            }
        }
        EXPECT_LE(samplers, kSamplersPerStage) << "stage bit " << stageBit << ": " << names;
    }
}

namespace
{
// The call sites in a SPIR-V module of the functions whose debug name starts with `name` and an
// opening parenthesis (glslang names a function by its mangled signature).
uint32_t SpirvCallsOf(const std::vector<uint8_t>& bytes, const std::string& name)
{
    constexpr uint32_t kOpName = 5;
    constexpr uint32_t kOpFunctionCall = 57;
    constexpr size_t kHeaderWords = 5;
    const size_t wordCount = bytes.size() / sizeof(uint32_t);
    std::vector<uint32_t> words(wordCount);
    std::memcpy(words.data(), bytes.data(), wordCount * sizeof(uint32_t));
    std::vector<uint32_t> functions;
    uint32_t calls = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (size_t at = kHeaderWords; at < wordCount;)
        {
            const uint32_t opcode = words[at] & 0xFFFFu;
            const uint32_t length = words[at] >> 16;
            if (length == 0)
                break;
            if (pass == 0 && opcode == kOpName && length >= 3)
            {
                const char* text = reinterpret_cast<const char*>(&words[at + 2]);
                if (std::string_view(text).starts_with(name + "("))
                    functions.push_back(words[at + 1]);
            }
            if (pass == 1 && opcode == kOpFunctionCall && length >= 4 &&
                std::find(functions.begin(), functions.end(), words[at + 3]) != functions.end())
                ++calls;
            at += length;
        }
    }
    return calls;
}
} // namespace

// The ocean samples the sun's cascade shadow once per fragment: the scatter tint samples it and the
// direct sun light reuses that value (GE_SurfaceSunShadow).
TEST_F(OceanSurfaceCompose, NativeProfileVariantSamplesTheSunShadowOnce)
{
    const MaterialBuildResult built = BuildOceanSurface(false);
    ASSERT_TRUE(built.success) << (built.errors.empty() ? std::string{} : built.errors.front());
    ASSERT_NE(built.package, nullptr);
    const auto fragment = built.package->stageBytes.find("fs");
    ASSERT_NE(fragment, built.package->stageBytes.end());
    EXPECT_EQ(SpirvCallsOf(fragment->second, "GE_SampleShadow"), 1u);
}
