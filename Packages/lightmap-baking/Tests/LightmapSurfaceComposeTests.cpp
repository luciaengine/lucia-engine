#include <gtest/gtest.h>
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include <atomic>
#include <fstream>
#include <map>

using namespace GameEngine;
using namespace GameEngine::Rendering;
TEST(LightmapSurfaceCompose, CompilesForwardClusteredAndMissingUVVariants)
{
    auto doc=MaterialDocument::CreateDefaultPBR("Baked GI");
    doc.surfaceShader="Surfaces/shadow_lightmap_pbr.glsl";
    doc.keywords.push_back("CUSTOM_LIGHTING");
    MaterialBuildContext context;
    context.AdapterShaderDir=TestPaths::StagedRenderingShadersDir();
    context.PackageShaderDirs.push_back(TestPaths::ExecutableDirectory()/"LightmapTestAssets/Shaders");
    static std::atomic<unsigned> sequence{0};
    context.CacheRoot=std::filesystem::temp_directory_path()/("ge-lightmap-compose-"+std::to_string(sequence++));
    for (bool uv : {false,true})
    for (auto flags : {MaterialKeyword::None, MaterialKeyword::Shadows,
        MaterialKeyword::ForwardPlus|MaterialKeyword::Shadows|MaterialKeyword::IBL,
        MaterialKeyword::ForwardPlus|MaterialKeyword::Shadows|MaterialKeyword::IBL|MaterialKeyword::DDGI})
    {
        SCOPED_TRACE(uv ? "UV1" : "No UV1");
        const auto result=BuildMaterialToShaderPackage(doc,context.AdapterShaderDir/"lightmap.material","Lightmap",context,ShaderSourceKind::SpirV,flags,
            VertexAttributeFlags::StandardMeshWithTangent|(uv?VertexAttributeFlags::HasUV1:VertexAttributeFlags::None));
        for (const auto& error : result.errors) ADD_FAILURE()<<error;
        ASSERT_TRUE(result.success);
        ASSERT_NE(result.package,nullptr);
        EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
    }
    std::error_code ec; std::filesystem::remove_all(context.CacheRoot,ec);
}
TEST(LightmapSurfaceCompose, AllEightTexturesHaveDistinctSlotsAndLinearBakeData)
{
    std::ifstream input(TestPaths::ExecutableDirectory()/"LightmapTestAssets/Shaders/Surfaces/shadow_lightmap_pbr.glsl");
    const std::string source((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    ASSERT_FALSE(source.empty());
    const auto slots=ShaderComposer::ResolveTextureSlots(source);
    EXPECT_FALSE(slots.Rejected) << slots.RejectReason;
    ASSERT_EQ(slots.DeclaredSlots.size(),8u);
    const std::map<std::string,uint8_t> ordinals(slots.DeclaredSlots.begin(),slots.DeclaredSlots.end());
    EXPECT_NE(ordinals.at("indirectLightmapMap"),ordinals.at("shadowLightmapMap"));
    EXPECT_GE(ordinals.at("indirectLightmapMap"),6u);
    EXPECT_GE(ordinals.at("shadowLightmapMap"),6u);
}
