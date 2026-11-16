// No build step compiles a lit surface with the virtual shadow keyword: the material pipeline
// composes it at run time. This test runs the standard surface through the real composer and
// compiler with and without the keyword.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <filesystem>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

class VirtualShadowSurfaceCompose : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_EngineShaderDir = TestPaths::StagedRenderingShadersDir();
        if (!fs::exists(m_EngineShaderDir))
            GTEST_SKIP() << "Staged engine shader tree not found: " << m_EngineShaderDir.string();
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";
        m_CacheRoot = fs::temp_directory_path() / ("ge_virtual_shadow_compose_" + GUID::Generate().ToString());
        fs::create_directories(m_CacheRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_CacheRoot, ec);
    }

    MaterialBuildResult BuildStandardSurface(MaterialKeyword passKeywords)
    {
        MaterialDocument document{};
        document.materialName = "VirtualShadowProbe";
        document.lightingModel = "StandardPBR";
        document.surfaceShader = "Surfaces/standard_pbr.glsl";
        MaterialBuildContext context{};
        context.AdapterShaderDir = m_EngineShaderDir;
        context.CacheRoot = m_CacheRoot / "Shaders";
        return BuildMaterialToShaderPackage(
            document, m_CacheRoot / "VirtualShadowProbe.material", "VirtualShadowProbe", context,
            ShaderSourceKind::SpirV,
            MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL | passKeywords,
            VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0);
    }

    fs::path m_EngineShaderDir;
    fs::path m_CacheRoot;
};

// shaderc runs at optimization level zero, so a GLSL function name stays in the SPIR-V as an OpName.
bool FragmentNames(const MaterialBuildResult& result, const std::string& name)
{
    if (!result.package)
        return false;
    const auto stage = result.package->stageBytes.find("fs");
    if (stage == result.package->stageBytes.end())
        return false;
    return std::search(stage->second.begin(), stage->second.end(), name.begin(), name.end()) !=
           stage->second.end();
}
} // namespace

TEST_F(VirtualShadowSurfaceCompose, StandardSurfaceHasThePageLookupsOnlyWithTheKeyword)
{
    const MaterialBuildResult conventional = BuildStandardSurface(MaterialKeyword::None);
    const MaterialBuildResult virtualShadows = BuildStandardSurface(MaterialKeyword::VirtualShadowMaps);
    for (const MaterialBuildResult* result : {&conventional, &virtualShadows})
    {
        for (const auto& error : result->errors)
            ADD_FAILURE() << "Build error: " << error;
        ASSERT_TRUE(result->success);
        ASSERT_NE(result->package, nullptr);
    }
    for (const char* lookup : {"GE_TryVirtualShadowDirectional", "GE_TryVirtualShadowLocal"})
    {
        EXPECT_FALSE(FragmentNames(conventional, lookup)) << lookup;
        EXPECT_TRUE(FragmentNames(virtualShadows, lookup)) << lookup;
    }
}

TEST_F(VirtualShadowSurfaceCompose, StandardSurfaceReadsTheSunMaskOnlyWithTheKeyword)
{
    const MaterialBuildResult lookup = BuildStandardSurface(MaterialKeyword::VirtualShadowMaps);
    const MaterialBuildResult sunMask =
        BuildStandardSurface(MaterialKeyword::VirtualShadowMaps | MaterialKeyword::VirtualShadowSunMask);
    for (const MaterialBuildResult* result : {&lookup, &sunMask})
    {
        for (const auto& error : result->errors)
            ADD_FAILURE() << "Build error: " << error;
        ASSERT_TRUE(result->success);
        ASSERT_NE(result->package, nullptr);
    }
    EXPECT_FALSE(FragmentNames(lookup, "GE_ReadVirtualShadowSunMask"));
    EXPECT_TRUE(FragmentNames(sunMask, "GE_ReadVirtualShadowSunMask"));
    // A fragment that is not the prepass receiver keeps the lookup in the shader.
    EXPECT_TRUE(FragmentNames(sunMask, "GE_TryVirtualShadowDirectional"));
}
