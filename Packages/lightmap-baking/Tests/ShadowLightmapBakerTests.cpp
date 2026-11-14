#include <gtest/gtest.h>
#include "Baker/ShadowLightmapBaker.h"
#include "Assets/ModelAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include <chrono>
#include "ModelAssetFbxTestAccess.h"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <algorithm>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
namespace
{
Mesh Quad(float xmin = -1, float xmax = 1, float y = 0)
{
    Mesh mesh{};
    for (auto p : {glm::vec3(xmin,y,-1), glm::vec3(xmax,y,-1), glm::vec3(xmax,y,1), glm::vec3(xmin,y,1)})
    {
        Vertex v{};
        v.Position[0] = p.x; v.Position[1] = p.y; v.Position[2] = p.z;
        v.Normal[1] = 1;
        mesh.Vertices.push_back(v);
    }
    mesh.Indices = {0,2,1,0,3,2};
    mesh.TexCoords1 = {0,0,1,0,1,1,0,1};
    return mesh;
}
ShadowLightmapBakeOptions Options()
{
    ShadowLightmapBakeOptions options;
    options.Width = options.Height = 16;
    options.Padding = 0;
    options.IndirectSamples = 0;
    return options;
}
}
TEST(ShadowLightmapBaker, UnoccludedAndSelfShadowAreFullyLit)
{
    auto mesh = Quad();
    ShadowLightmapMesh receiver{&mesh};
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap(receiver, {}, Options(), result, error)) << error;
    EXPECT_EQ(result.CoveredTexels, 256u);
    EXPECT_EQ(result.TracedRays, 1024u);
    for (auto v : result.Visibility) EXPECT_EQ(v, 255);
    ASSERT_TRUE(BakeShadowLightmap(receiver, {&receiver,1}, Options(), result, error)) << error;
    for (auto v : result.Visibility) EXPECT_EQ(v, 255);
}
TEST(ShadowLightmapBaker, ProgressIsMonotonicThroughDenoisingAndPadding)
{
    auto mesh=Quad();
    auto options=Options(); options.Padding=2; options.IndirectSamples=4;
    options.EnvironmentRadiance=glm::vec3(0.2f);
    std::vector<float> progress;
    options.Progress=[&](float value) { progress.push_back(value); };
    ShadowLightmapBakeResult result; std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},options,result,error))<<error;
    ASSERT_GT(progress.size(),10u);
    EXPECT_EQ(progress.front(),0);
    EXPECT_EQ(progress.back(),1);
    EXPECT_TRUE(std::is_sorted(progress.begin(),progress.end()));
    EXPECT_TRUE(std::any_of(progress.begin(),progress.end(),[](float p) { return p>=0.9f && p<0.98f; }));
    EXPECT_TRUE(std::any_of(progress.begin(),progress.end(),[](float p) { return p>=0.98f && p<1; }));
}
TEST(ShadowLightmapBaker, CancelledProgressDoesNotReportSuccess)
{
    auto mesh=Quad(); auto options=Options();
    float progress=0;
    options.Progress=[&](float value) { progress=value; };
    options.Continue=[&] { return progress<0.3f; };
    ShadowLightmapBakeResult result; result.Width=7;
    std::string error;
    EXPECT_FALSE(BakeShadowLightmap({&mesh},{},options,result,error));
    EXPECT_LT(progress,1);
    EXPECT_EQ(result.Width,7u);
}
TEST(ShadowLightmapBaker, OpaqueBlockerCastsAWorldSpaceShadowIntoUV1)
{
    auto mesh = Quad(), blocker = Quad(-1, 0, 1);
    std::array sources{ShadowLightmapMesh{&mesh}, ShadowLightmapMesh{&blocker}};
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap(sources[0], sources, Options(), result, error)) << error;
    for (unsigned y=0; y<16; ++y)
        for (unsigned x=0; x<16; ++x)
            EXPECT_EQ(result.Visibility[y*16+x], x<8 ? 0 : 255) << x << ',' << y;
}
TEST(ShadowLightmapBaker, TransformsAndMirroredWindingDoNotChangeVisibility)
{
    auto mesh = Quad(), blocker = Quad(-1, 0, 1);
    auto transform = glm::translate(glm::mat4(1), glm::vec3(100,20,-70));
    transform = glm::rotate(transform, 0.6f, glm::vec3(0,0,1));
    transform = glm::scale(transform, glm::vec3(-2,3,0.5f));
    std::array sources{ShadowLightmapMesh{&mesh,transform}, ShadowLightmapMesh{&blocker,transform}};
    auto options = Options();
    options.DirectionToLight = glm::vec3(transform * glm::vec4(0,1,0,0));
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap(sources[0], sources, options, result, error)) << error;
    EXPECT_EQ(result.Visibility[8*16+3], 0);
    EXPECT_EQ(result.Visibility[8*16+12], 255);
}
TEST(ShadowLightmapBaker, SoftShadowsHaveDeterministicPenumbrae)
{
    auto mesh = Quad(), blocker = Quad(-2, 0, 1);
    ShadowLightmapMesh source{&blocker};
    auto options = Options(); options.AngularRadiusRadians = 0.2f; options.RaysPerSample = 32;
    ShadowLightmapBakeResult a,b;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, a, error)) << error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, b, error)) << error;
    EXPECT_EQ(a.Visibility, b.Visibility);
    EXPECT_EQ(a.TracedRays, 32768u);
    EXPECT_GT(a.Visibility[8*16+7], 0);
    EXPECT_LT(a.Visibility[8*16+7], 255);
    EXPECT_GT(a.Visibility[8*16+8], 0);
    EXPECT_LT(a.Visibility[8*16+8], 255);
    EXPECT_EQ(a.Visibility[8*16+2], 0);
    EXPECT_EQ(a.Visibility[8*16+13], 255);
}
TEST(ShadowLightmapBaker, PaddingFillsOnlyEmptyTexels)
{
    auto mesh = Quad(), blocker = Quad(-2,2,1);
    for (auto& uv : mesh.TexCoords1) uv = uv * 0.5f + 0.25f;
    ShadowLightmapMesh source{&blocker};
    auto options = Options(); options.Padding = 2;
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, result, error)) << error;
    EXPECT_EQ(result.CoveredTexels, 64u);
    EXPECT_EQ(result.Visibility[8*16+4], 0);
    EXPECT_EQ(result.Visibility[8*16+2], 0);
    EXPECT_EQ(result.Visibility[8*16+1], 255);
}
TEST(ShadowLightmapBaker, InvalidInputAndCancellationPreservePreviousOutput)
{
    auto mesh = Quad();
    ShadowLightmapBakeResult result; result.Visibility = {42};
    std::string error;
    auto options = Options(); options.Width = 0;
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, options, result, error));
    EXPECT_FALSE(error.empty());
    options = Options(); options.DirectionToLight = glm::vec3(0);
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, options, result, error));
    options = Options(); options.Continue = [] { return false; };
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, options, result, error));
    EXPECT_NE(error.find("cancelled"), std::string::npos);
    mesh.Indices[0] = 999;
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, Options(), result, error));
    EXPECT_EQ(result.Visibility, std::vector<uint8_t>{42});
}
TEST(ShadowLightmapBaker, MissingDegenerateAndOverlappingUVsAreRejected)
{
    auto mesh = Quad();
    ShadowLightmapBakeResult result;
    std::string error;
    mesh.TexCoords1.clear();
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, Options(), result, error));
    mesh = Quad(); mesh.TexCoords1.assign(8,0);
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, Options(), result, error));
    mesh = Quad(); mesh.Indices.insert(mesh.Indices.end(), {0,2,1});
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, Options(), result, error));
    EXPECT_NE(error.find("Overlapping"), std::string::npos);
    mesh = Quad(); mesh.TexCoords1[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, Options(), result, error));
}
TEST(ShadowLightmapBaker, BlendModesMatchGodotEquations)
{
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,0,ShadowLightmapMode::Replace),0.4f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,1,ShadowLightmapMode::Replace),0.2f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,0.5f,ShadowLightmapMode::Replace),0.3f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,0,ShadowLightmapMode::Overlay),0.08f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,1,ShadowLightmapMode::Overlay),0.2f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,0.5f,ShadowLightmapMode::Overlay),0.14f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,0,ShadowLightmapMode::Only),0.2f);
    EXPECT_FLOAT_EQ(BlendShadowLightmap(0.4f,0.2f,1,ShadowLightmapMode::None),0.4f);
}

TEST(ShadowLightmapBaker, UniformEnvironmentHasCorrectIrradianceNormalization)
{
    auto mesh = Quad();
    auto options = Options(); options.IndirectSamples = 32;
    options.EnvironmentRadiance = {0.2f,0.5f,2.0f};
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {}, options, result, error)) << error;
    ASSERT_EQ(result.IndirectRGB.size(), 256u*3);
    for (unsigned i=0; i<256; ++i)
        for (unsigned c=0; c<3; ++c)
            EXPECT_NEAR(result.IndirectRGB[i*3+c], options.EnvironmentRadiance[c], 1e-5f);
}
TEST(ShadowLightmapBaker, StratifiedHemisphereResolvesAConstantHalfSkyWithoutNoise)
{
    auto mesh=Quad();
    auto options=Options(); options.IndirectSamples=16; options.Denoise=false;
    options.SampleEnvironment=[](glm::vec3 d) { return glm::vec3(d.x>0 ? 1.0f : 0.0f); };
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},options,result,error))<<error;
    // Exactly half the cosine hemisphere sees the bright half of this sky.
    for (float v : result.IndirectRGB) EXPECT_NEAR(v,0.5f,1e-6f);
}

TEST(ShadowLightmapBaker, DenoisingReducesSmallEmitterErrorAgainstHighSampleReference)
{
    auto mesh=Quad(), emitter=Quad(-0.25f,0.25f,1);
    for (auto& v : emitter.Vertices) v.Position[2]*=0.25f;
    ShadowLightmapMesh source{&emitter}; source.Surface={glm::vec3(0),glm::vec3(8,1,0.2f)};
    auto options=Options(); options.Width=options.Height=32;
    options.LightRadiance=glm::vec3(0); options.IndirectBounces=1;
    options.IndirectSamples=1024; options.Denoise=false;
    ShadowLightmapBakeResult reference,raw,filtered;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,reference,error))<<error;
    options.IndirectSamples=8;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,raw,error))<<error;
    options.Denoise=true;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,filtered,error))<<error;
    double rawError=0, filteredError=0, referenceSum=0, filteredSum=0;
    for (size_t i=0; i<reference.IndirectRGB.size(); i+=3)
    {
        rawError+=std::pow(raw.IndirectRGB[i]-reference.IndirectRGB[i],2);
        filteredError+=std::pow(filtered.IndirectRGB[i]-reference.IndirectRGB[i],2);
        referenceSum+=reference.IndirectRGB[i]; filteredSum+=filtered.IndirectRGB[i];
    }
    EXPECT_LT(filteredError,rawError*0.6);
    EXPECT_NEAR(filteredSum,referenceSum,referenceSum*0.1);
    EXPECT_EQ(filtered.Visibility,raw.Visibility);
    EXPECT_EQ(filtered.TracedRays,raw.TracedRays);
}

TEST(ShadowLightmapBaker, DenoisingDoesNotLeakBetweenSeparatedCharts)
{
    auto mesh=Quad(-1,0), upper=Quad(0,1,100), ceiling=Quad(-10000,10000,1);
    for (auto& v : ceiling.Vertices) v.Position[2]*=10000;
    for (size_t i=0; i<4; ++i)
    {
        mesh.TexCoords1[i*2]*=0.5f;
        upper.TexCoords1[i*2]=0.5f+upper.TexCoords1[i*2]*0.5f;
    }
    mesh.Vertices.insert(mesh.Vertices.end(),upper.Vertices.begin(),upper.Vertices.end());
    mesh.TexCoords1.insert(mesh.TexCoords1.end(),upper.TexCoords1.begin(),upper.TexCoords1.end());
    for (auto index : upper.Indices) mesh.Indices.push_back(index+4);
    ShadowLightmapMesh source{&ceiling}; source.Surface={glm::vec3(0),glm::vec3(2)};
    auto options=Options(); options.IndirectSamples=16; options.Denoise=true;
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,result,error))<<error;
    for (size_t y=0; y<16; ++y)
        for (size_t x=0; x<16; ++x)
            EXPECT_NEAR(result.IndirectRGB[(y*16+x)*3],x<8 ? 2.0f : 0.0f,1e-5f);
}

TEST(ShadowLightmapBaker, CancellationDuringDenoisingPreservesPreviousOutput)
{
    auto mesh=Quad(); auto options=Options();
    options.IndirectSamples=4; options.EnvironmentRadiance=glm::vec3(1); options.Denoise=false;
    unsigned calls=0;
    options.Continue=[&] { ++calls; return true; };
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},options,result,error))<<error;
    const auto previous=result.IndirectRGB;
    const unsigned stopAt=calls+3;
    calls=0; options.Denoise=true; options.EnvironmentRadiance=glm::vec3(2);
    options.Continue=[&] { return ++calls<=stopAt; };
    EXPECT_FALSE(BakeShadowLightmap({&mesh},{},options,result,error));
    EXPECT_NE(error.find("cancelled"),std::string::npos);
    EXPECT_EQ(result.IndirectRGB,previous);
}
TEST(ShadowLightmapBaker, EmissiveCeilingLightsReceiverAndRetainsHDR)
{
    auto mesh = Quad(), ceiling = Quad(-1000,1000,1);
    for (auto& v : ceiling.Vertices) v.Position[2] *= 1000;
    ShadowLightmapMesh source{&ceiling};
    source.Surface = {glm::vec3(0), glm::vec3(4,1,0.25f)};
    auto options = Options(); options.IndirectSamples = 64;
    options.LightRadiance = glm::vec3(0);
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, result, error)) << error;
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3],4,0.02f);
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3+1],1,0.02f);
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3+2],0.25f,0.02f);
}
TEST(ShadowLightmapBaker, DiffuseBounceTransfersSurfaceColor)
{
    auto mesh = Quad(), ceiling = Quad(-1000,1000,1);
    for (auto& v : ceiling.Vertices) v.Position[2] *= 1000;
    ShadowLightmapMesh source{&ceiling}; source.Surface.Albedo = {0.8f,0.05f,0.02f};
    auto options = Options(); options.IndirectSamples = 64;
    // Light points upward into the downward-facing bounce surface.
    options.DirectionToLight = {0,-1,0}; options.LightRadiance = glm::vec3(3.14159265359f);
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, result, error)) << error;
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3],0.8f,0.01f);
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3+1],0.05f,0.01f);
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3+2],0.02f,0.01f);
}
TEST(ShadowLightmapBaker, MaterialSamplerControlsEmissiveBounce)
{
    auto mesh = Quad(), ceiling = Quad(-1000,1000,1);
    for (auto& v : ceiling.Vertices) v.Position[2] *= 1000;
    ShadowLightmapMesh source{&ceiling};
    source.SampleSurface = [](glm::vec2) { return LightmapSurface{glm::vec3(0),glm::vec3(0,3,0)}; };
    auto options = Options(); options.IndirectSamples = 32;
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {&source,1}, options, result, error)) << error;
    EXPECT_FLOAT_EQ(result.IndirectRGB[(8*16+8)*3],0);
    EXPECT_NEAR(result.IndirectRGB[(8*16+8)*3+1],3,0.02f);
}
TEST(ShadowLightmapBaker, EnvironmentSamplerAndInvalidRadiance)
{
    auto mesh = Quad();
    auto options = Options(); options.IndirectSamples = 16;
    options.SampleEnvironment = [](glm::vec3 d) { return d.y>0 ? glm::vec3(2,0,0) : glm::vec3(0,0,3); };
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh}, {}, options, result, error)) << error;
    EXPECT_FLOAT_EQ(result.IndirectRGB[0],2);
    EXPECT_FLOAT_EQ(result.IndirectRGB[2],0);
    const auto previous = result.IndirectRGB;
    options.SampleEnvironment = [](glm::vec3) { return glm::vec3(std::numeric_limits<float>::quiet_NaN()); };
    EXPECT_FALSE(BakeShadowLightmap({&mesh}, {}, options, result, error));
    EXPECT_EQ(result.IndirectRGB,previous);
}
TEST(ShadowLightmapBaker, PointAndSpotLightsContributeToIndirectBounce)
{
    auto mesh = Quad(), ceiling = Quad(-2,2,1);
    ShadowLightmapMesh source{&ceiling}; source.Surface.Albedo=glm::vec3(1);
    auto options=Options(); options.IndirectSamples=64; options.LightRadiance=glm::vec3(0);
    LightmapBounceLight light; light.Position={0,0,0}; light.Radiance={10,0,0};
    options.BounceLights.push_back(light);
    ShadowLightmapBakeResult point,spot;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,point,error))<<error;
    EXPECT_GT(point.IndirectRGB[(8*16+8)*3],0.1f);
    EXPECT_FLOAT_EQ(point.IndirectRGB[(8*16+8)*3+1],0);
    options.BounceLights[0].Kind=LightmapBounceLight::Type::Spot;
    options.BounceLights[0].Direction={0,-1,0}; // away from the ceiling
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,spot,error))<<error;
    EXPECT_FLOAT_EQ(spot.IndirectRGB[(8*16+8)*3],0);
}
TEST(ShadowLightmapBaker, MirroredReceiverUsesTransformedNormalsForGI)
{
    auto mesh=Quad();
    auto options=Options(); options.IndirectSamples=16;
    options.SampleEnvironment=[](glm::vec3 d) { return glm::vec3(d.y>0?1:0); };
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh,glm::scale(glm::mat4(1),glm::vec3(-1,1,1))},{},options,result,error))<<error;
    EXPECT_FLOAT_EQ(result.IndirectRGB[0],1);
}

// Package settings use the existing generic vector lanes, so integer JSON
// components still become float values without core lightmap property names.
TEST(ShadowLightmapBaker, PackageUserLanesSurviveIntegerJSONAndTextureReferences)
{
    const std::string source=R"({"schemaVersion":3,"materialName":"Bake","lightingModel":"StandardPBR",
        "surfaceShader":"Surfaces/shadow_lightmap_pbr.glsl",
        "properties":{"userVec0":[2,40,50,1],"userVec1":[0,1,0,0]},
        "textures":{"shadowLightmapMap":{"guid":"11111111-2222-3333-4444-555555555555","path":"Lightmaps/test_shadow.png"}}})";
    MaterialAsset asset(GUID::Generate(),"bake.material");
    ASSERT_TRUE(asset.LoadFromData(Vector<uint8>(source.begin(),source.end())));
    const auto& doc=asset.GetDocument();
    EXPECT_EQ(doc.textures.at("shadowLightmapMap"),"11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(doc.texturePaths.at("shadowLightmapMap"),"Lightmaps/test_shadow.png");
    ASSERT_TRUE(std::holds_alternative<std::vector<float>>(doc.properties.at("userVec0")));
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("userVec0")),
              (std::vector<float>{2, 40, 50, 1}));
    EXPECT_EQ(std::get<std::vector<float>>(doc.properties.at("userVec1")),
              (std::vector<float>{0, 1, 0, 0}));
}

TEST(ShadowLightmapBaker, GltfSecondUVChannelSurvivesImport)
{
    const std::string json=R"GLTF({"asset":{"version":"2.0"},"buffers":[{"byteLength":172,"uri":"data:application/octet-stream;base64,AACAvwAAAAAAAIC/AACAPwAAAAAAAIC/AACAPwAAAAAAAIA/AACAvwAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD/NzMw9zczMPWZmZj/NzMw9ZmZmP2ZmZj/NzMw9ZmZmPwAAAgABAAAAAwACAA=="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":32},{"buffer":0,"byteOffset":160,"byteLength":12}],"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,0,-1],"max":[1,0,1]},{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":4,"componentType":5123,"count":6,"type":"SCALAR"}],"meshes":[{"name":"LightmapQuad","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"TEXCOORD_1":3},"indices":4}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})GLTF";
    ModelAsset asset(GUID::Generate(),"lightmap.gltf");
    ASSERT_TRUE(ModelAssetFbxTestAccess::LoadGltf(asset,Vector<uint8>(json.begin(),json.end()),{}));
    ASSERT_EQ(asset.GetMeshCount(),1u);
    const auto& mesh=asset.GetMesh(0);
    ASSERT_TRUE(mesh.HasTexCoords1());
    EXPECT_NEAR(mesh.TexCoords1[0],0.1f,1e-6f);
    EXPECT_NEAR(mesh.TexCoords1[1],0.1f,1e-6f);
    EXPECT_NEAR(mesh.TexCoords1[4],0.9f,1e-6f);
    EXPECT_NEAR(mesh.TexCoords1[5],0.9f,1e-6f);
    EXPECT_NEAR(mesh.Vertices[0].TexCoords[0],0.0f,1e-6f);
    ShadowLightmapBakeResult result;
    std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},Options(),result,error))<<error;
}

TEST(ShadowLightmapBaker, SupportsEightKDimensionsAndRejectsLargerTextures)
{
    auto mesh=Quad(); auto options=Options();
    options.Width=kMaxLightmapResolution; options.Height=1;
    ShadowLightmapBakeResult result; std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},options,result,error))<<error;
    EXPECT_EQ(result.Width,8192u); EXPECT_EQ(result.CoveredTexels,8192u);
    EXPECT_TRUE(std::all_of(result.Visibility.begin(),result.Visibility.end(),[](uint8_t value) { return value==255; }));
    options.Width=1; options.Height=kMaxLightmapResolution;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{},options,result,error))<<error;
    EXPECT_EQ(result.Height,8192u); EXPECT_EQ(result.CoveredTexels,8192u);
    options.Height=kMaxLightmapResolution+1;
    EXPECT_FALSE(BakeShadowLightmap({&mesh},{},options,result,error));
    EXPECT_EQ(result.Height,8192u);
    options.Height=1; options.Width=kMaxLightmapResolution+1;
    EXPECT_FALSE(BakeShadowLightmap({&mesh},{},options,result,error));
}

TEST(ShadowLightmapBaker, FailedAccelerationRetriesBatchOnCpuWithoutChangingOutput)
{
    auto mesh=Quad(), blocker=Quad(-2,0,1);
    ShadowLightmapMesh source{&blocker}; source.Surface.Emission={2,0.3f,0.1f};
    auto options=Options(); options.IndirectSamples=16; options.IndirectBounces=3;
    options.AngularRadiusRadians=0.2f; options.EnvironmentRadiance=glm::vec3(0.2f);
    ShadowLightmapBakeResult cpu,fallback; std::string error;
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,cpu,error))<<error;
    unsigned calls=0; std::vector<bool> states;
    options.CreateRayTracer=[&](std::span<const float> positions) -> LightmapRayBatch {
        EXPECT_EQ(positions.size(),18u);
        return [&](std::span<const LightmapRay> rays,std::span<LightmapRayHit> hits) {
            ++calls; EXPECT_GT(rays.size(),1u); EXPECT_LE(rays.size(),65536u);
            // Partial/corrupt results from a failed submission must be discarded.
            std::fill(hits.begin(),hits.end(),LightmapRayHit{0,0,0,12345});
            return false;
        };
    };
    options.AccelerationChanged=[&](bool gpu) { states.push_back(gpu); };
    ASSERT_TRUE(BakeShadowLightmap({&mesh},{&source,1},options,fallback,error))<<error;
    EXPECT_EQ(calls,1u); EXPECT_EQ(states,(std::vector<bool>{true,false}));
    EXPECT_EQ(fallback.Visibility,cpu.Visibility); EXPECT_EQ(fallback.IndirectRGB,cpu.IndirectRGB);
    EXPECT_EQ(fallback.TracedRays,cpu.TracedRays); EXPECT_EQ(fallback.GpuTracedRays,0u);
}

TEST(ShadowLightmapBaker, CancelledAccelerationPreservesOutput)
{
    auto mesh=Quad(); ShadowLightmapMesh source{&mesh};
    // A shadow-only one-texel bake finishes in a single GPU batch. A cancel
    // arriving during that last dispatch must still prevent output publication.
    auto options=Options(); options.Width=options.Height=1;
    bool cancelled=false;
    options.Continue=[&] { return !cancelled; };
    options.CreateRayTracer=[&](std::span<const float>) -> LightmapRayBatch {
        return [&](std::span<const LightmapRay>,std::span<LightmapRayHit> hits) {
            std::fill(hits.begin(),hits.end(),LightmapRayHit{});
            cancelled=true;
            return true;
        };
    };
    ShadowLightmapBakeResult result; result.Visibility={42}; std::string error;
    EXPECT_FALSE(BakeShadowLightmap(source,{&source,1},options,result,error));
    EXPECT_NE(error.find("cancelled"),std::string::npos);
    EXPECT_EQ(result.Visibility,(std::vector<uint8_t>{42}));
}

class LightmapGpuRayTracing : public ::testing::Test
{
protected:
    void SetUp() override
    {
        previousRoot=ShaderProgramAsset::GetShaderCacheRoot();
        cacheRoot=std::filesystem::temp_directory_path() /
            ("ge-lightmap-gpu-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ShaderProgramAsset::SetShaderCacheRoot(cacheRoot);
    }
    void TearDown() override
    {
        ShaderProgramAsset::SetShaderCacheRoot(previousRoot);
        std::error_code error;
        std::filesystem::remove_all(cacheRoot,error);
    }
    std::filesystem::path previousRoot,cacheRoot;
};

TEST_F(LightmapGpuRayTracing, ClosestHitsBarycentricsClippingAndDoubleSidedTriangles)
{
    const std::vector<float> positions={0,1,0, 1,1,0, 0,1,1, 0,2,0, 1,2,0, 0,2,1};
    auto trace=CreateGpuLightmapRayTracer(positions);
    if (!trace) GTEST_SKIP()<<"GPU ray queries unavailable";
    const std::vector<LightmapRay> rays={
        {{0.25f,0,0.25f},100,{0,1,0}},
        {{0.25f,0,0.25f},0.5f,{0,1,0}},
        {{0.25f,3,0.25f},100,{0,-1,0}},
        {{2,0,2},100,{0,1,0}},
        {{0.25f,0,0.25f},100,{0,1,0},1.1f}};
    std::vector<LightmapRayHit> hits(rays.size());
    for (int pass=0; pass<8; ++pass) // cross frame-slot reuse and descriptor lifetime
    {
        ASSERT_TRUE(trace(rays,hits));
        EXPECT_EQ(hits[0].Triangle,0u); EXPECT_NEAR(hits[0].Distance,1,1e-5f);
        EXPECT_NEAR(hits[0].U,0.25f,1e-5f); EXPECT_NEAR(hits[0].V,0.25f,1e-5f);
        EXPECT_EQ(hits[1].Triangle,UINT32_MAX);
        EXPECT_EQ(hits[2].Triangle,1u); EXPECT_NEAR(hits[2].Distance,1,1e-5f);
        EXPECT_EQ(hits[3].Triangle,UINT32_MAX);
        EXPECT_EQ(hits[4].Triangle,1u); EXPECT_NEAR(hits[4].Distance,2,1e-5f);
    }
    EXPECT_TRUE(trace({},{}));
}

TEST_F(LightmapGpuRayTracing, TexturedMultiBounceLightingMatchesCpu)
{
    auto mesh=Quad(), ceiling=Quad(-1,0.3f,1);
    for (size_t i=0; i<ceiling.Vertices.size(); ++i)
    {
        ceiling.Vertices[i].TexCoords[0]=ceiling.TexCoords1[i*2];
        ceiling.Vertices[i].TexCoords[1]=ceiling.TexCoords1[i*2+1];
    }
    std::array sources{ShadowLightmapMesh{&mesh},ShadowLightmapMesh{&ceiling}};
    sources[1].SampleSurface=[](glm::vec2 uv) {
        return LightmapSurface{glm::vec3(0.6f,0.1f+uv.x*0.2f,0.05f),glm::vec3(uv.y,0.2f,0)};
    };
    auto options=Options(); options.IndirectSamples=32; options.IndirectBounces=3;
    options.AngularRadiusRadians=0.15f; options.Denoise=false;
    options.SampleEnvironment=[](glm::vec3 d) { return glm::vec3(0.1f,0.2f,0.3f)*std::max(0.0f,d.y); };
    LightmapBounceLight lamp; lamp.Position={0,0.5f,0}; lamp.Radiance={4,2,1};
    options.BounceLights.push_back(lamp);
    ShadowLightmapBakeResult cpu,gpu; std::string error;
    ASSERT_TRUE(BakeShadowLightmap(sources[0],sources,options,cpu,error))<<error;
    bool available=false;
    options.CreateRayTracer=[&](std::span<const float> positions) {
        auto tracer=CreateGpuLightmapRayTracer(positions); available=bool(tracer); return tracer;
    };
    ASSERT_TRUE(BakeShadowLightmap(sources[0],sources,options,gpu,error))<<error;
    if (!available) GTEST_SKIP()<<"GPU ray queries unavailable";
    ASSERT_GT(gpu.GpuTracedRays,0u); EXPECT_EQ(gpu.GpuTracedRays,gpu.TracedRays);
    EXPECT_EQ(gpu.Visibility,cpu.Visibility);
    ASSERT_EQ(gpu.IndirectRGB.size(),cpu.IndirectRGB.size());
    double difference=0;
    for (size_t i=0; i<gpu.IndirectRGB.size(); ++i) difference+=std::abs(gpu.IndirectRGB[i]-cpu.IndirectRGB[i]);
    EXPECT_LT(difference/gpu.IndirectRGB.size(),0.002);
}

TEST(ShadowLightmapBaker, MidBakeGpuFailurePreservesCompletedRaysAndContinuesOnCpu)
{
    auto mesh=Quad(); ShadowLightmapMesh source{&mesh};
    auto options=Options(); options.IndirectSamples=16;
    options.EnvironmentRadiance={0.2f,0.4f,0.8f};
    ShadowLightmapBakeResult cpu,mixed; std::string error;
    ASSERT_TRUE(BakeShadowLightmap(source,{&source,1},options,cpu,error))<<error;
    unsigned calls=0; uint64_t gpuRays=0;
    options.CreateRayTracer=[&](std::span<const float>) -> LightmapRayBatch {
        return [&](std::span<const LightmapRay> rays,std::span<LightmapRayHit> hits) {
            if (++calls>1) return false;
            // The first upward visibility rays cannot hit their own flat floor.
            std::fill(hits.begin(),hits.end(),LightmapRayHit{});
            gpuRays=rays.size();
            return true;
        };
    };
    ASSERT_TRUE(BakeShadowLightmap(source,{&source,1},options,mixed,error))<<error;
    EXPECT_EQ(calls,2u); EXPECT_GT(gpuRays,0u);
    EXPECT_EQ(mixed.GpuTracedRays,gpuRays); EXPECT_LT(mixed.GpuTracedRays,mixed.TracedRays);
    EXPECT_EQ(mixed.Visibility,cpu.Visibility); EXPECT_EQ(mixed.IndirectRGB,cpu.IndirectRGB);
    EXPECT_EQ(mixed.TracedRays,cpu.TracedRays);
}
