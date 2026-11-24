#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ReverseZ.h"
#include "Rendering/Materials/Shader/ShaderCompileService.h"
#include "ScopedEnvVar.h"
#include "TestTempDir.h"
#include "TestUtils.h"

#include <array>
#include <cmath>

using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 4;

const char* kVertex = R"glsl(#version 450
#define GE_STAGE_VERTEX
#include "Includes/impostor_depth.glsl"
void main()
{
    const vec2 corners[] = vec2[](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    mat3 linear = mat3(1.0);
#if defined(PROBE_PERSPECTIVE) || defined(PROBE_RADIAL)
    linear = mat3(vec3(2.4,0,-1.8), vec3(0,0.5,0), vec3(0.6,0,0.8));
#endif
    GE_SetImpostorDepthFrame(linear, 2.0);
    gl_Position = vec4(corners[gl_VertexIndex], 0.5, 1.0);
}
)glsl";

// Fixed atlas samples exercise both signs, empty coverage and a premultiplied leaf edge.
// The production surface selects the samples, reconstructs the hit and writes the attachment.
const char* kFragment = R"glsl(#version 450
#define ALPHA_TEST
struct Camera {
#include "Includes/camera_ubo_fields.glsl"
};
Camera Cam;
#include "Includes/camera_relative.glsl"
struct Properties {vec4 impostorAtlas; float alphaCutoff; float impostorNormalCoherenceWeight;};
#ifdef PROBE_DEFERRED_COLOR
const Properties Props = Properties(vec4(2,0,0,1), 0.25, 1);
#else
const Properties Props = Properties(vec4(2,0,0,0), 0.25, 1);
#endif
struct SurfaceInput {vec2 uv0; vec4 custom0; vec3 positionRelWS;};
struct SurfaceOutput {vec3 baseColor; vec3 normalWS; float opacity; float roughness; float metallic;};
SurfaceOutput DefaultSurfaceOutput() {return SurfaceOutput(vec3(0),vec3(0),0,0,0);}
const int albedoMap = 0, normalMap = 1;
vec4 ReadAtlas(int atlas, vec2 uv)
{
    int band = clamp(int(uv.x * 8.0), 0, 3);
    const vec4 coverage = vec4(1, 1, 0, 0.5);
    const vec4 bakedDepth = vec4(0.75, 0.25, 0, 0.375);
    return vec4(vec3(0.5), atlas == albedoMap ? coverage[band] : bakedDepth[band]);
}
#define texture(atlas, uv) ReadAtlas(atlas, uv)
#define textureLod(atlas, uv, lod) ReadAtlas(atlas, uv)
#define textureSize(atlas, lod) ivec2(64,64)
#include "Surfaces/impostor.glsl"
void main()
{
    Cam.uCameraPos = vec4(0);
    Cam.uVPRel = mat4(1);
    Cam.uVPRel[2].z = -0.25;
    Cam.uVPRel[3].z = 0.5;
#ifdef PROBE_PERSPECTIVE
    Cam.uVPRel[2] = vec4(0,0,0,1);
    Cam.uVPRel[3] = vec4(0,0,2,4);
#elif defined(PROBE_RADIAL)
    Cam.uCameraPos.w = -8.0;
    Cam.uVRel = mat4(1);
    Cam.uVRel[3].z = 4.0;
#endif
    SurfaceInput inputPoint = SurfaceInput(vec2(gl_FragCoord.x / 64.0, 0.5),vec4(0,0,1,0),vec3(0));
    SurfaceOutput outputPoint = EvaluateSurface(inputPoint);
#ifndef PROBE_DEFERRED_COLOR
    if (outputPoint.opacity < Props.alphaCutoff) discard;
#endif
}
)glsl";

struct DepthCase
{
    const char* Define;
    std::array<float, 4> Expected;
    bool DeferColorCoverage = false;
};
}

TEST(ImpostorDepthReadback, ColorAndPrepassWriteTheSignedBakedDepth)
{
    const Tests::ScopedEnvVar headless("GE_HEADLESS_TEST", "1");
    DeviceDesc deviceDesc{};
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "Headless graphics device unavailable";

    const auto cache = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_impostor_depth_readback");
    const DepthCase cases[] = {
        {nullptr, {0.75f, 0.25f, 0.0f, 0.625f}},
        {"PROBE_DEFERRED_COLOR", {0.75f, 0.25f, 0.0f, 0.625f}, true},
        {"PROBE_PERSPECTIVE", {2.0f / 3.2f, 2.0f / 4.8f, 0.0f, 2.0f / 3.6f}},
        {"PROBE_RADIAL", {1.0f - std::hypot(0.6f, 3.2f) / 8.0f, 1.0f - std::hypot(0.6f, 4.8f) / 8.0f,
                          0.0f, 1.0f - std::hypot(0.3f, 3.6f) / 8.0f}}
    };
    for (const DepthCase& depthCase : cases)
    {
        SCOPED_TRACE(depthCase.Define ? depthCase.Define : "orthographic");
        const std::array<const char*, 3> passes{nullptr, "GE_DEPTH_ONLY_FRAGMENT", "GE_MOTION_VECTORS"};
        for (const char* passDefine : passes)
        {
            SCOPED_TRACE(passDefine ? passDefine : "color");
            ShaderProgramCompileRequest request{};
            request.debugName = "ImpostorDepthProbe";
            request.cacheRoot = cache;
            request.baseDirectory = Tests::GetAdapterShaderDir();
            request.includeDirs = {request.baseDirectory};
            ShaderStageCompileSpec vertex{};
            vertex.stage = "vs";
            vertex.sourcePath = "impostor-depth-probe.vert";
            vertex.inlineSource = kVertex;
            ShaderStageCompileSpec fragment{};
            fragment.stage = "fs";
            fragment.sourcePath = "impostor-depth-probe.frag";
            fragment.inlineSource = kFragment;
            if (passDefine)
                fragment.defines = {passDefine};
            if (depthCase.Define)
            {
                vertex.defines.push_back(depthCase.Define);
                fragment.defines.push_back(depthCase.Define);
            }
            request.stages = {vertex, fragment};
            ShaderProgramCompileResult program{};
            std::string error;
            ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, program, &error)) << error;
            PipelineDesc desc{};
            desc.type = PipelineType::Graphics;
            desc.vertexShader = program.stageBytes.at("vs");
            desc.pixelShader = program.stageBytes.at("fs");
            desc.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
            desc.rasterizationState.cullMode = CullModeFlagBits::None;
            desc.EnableDepthTest(true);
            desc.depthStencilState.depthWriteEnable = true;
            const auto pipeline = device->CreatePipeline(desc);
            ASSERT_TRUE(pipeline.IsValid());
            TextureDesc target{};
            target.width = kWidth;
            target.height = kHeight;
            target.format = desc.depthAttachmentFormat;
            target.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::TransferSrc);
            const auto depth = device->CreateTexture(target);
            const auto readback = device->CreateReadbackBuffer(kWidth * kHeight * sizeof(float));
            ASSERT_TRUE(depth.IsValid() && readback.IsValid());

            for (float underlyingDepth : {ReverseZ::kClearFar, 0.5f})
            {
                auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
                commands->Begin();
                commands->Barrier(ResourceBarrier::CreateTextureBarrier(depth, ResourceState::Undefined, ResourceState::DepthWrite));
                RenderPassDesc pass{};
                pass.colorTargetCount = 0;
                pass.depthTarget = depth;
                pass.clearDepth = true;
                pass.clearDepthValue = underlyingDepth;
                pass.depthStoreOp = RenderPassDesc::StoreOp::Store;
                commands->BeginRenderPass(pass);
                commands->SetPipeline(pipeline);
                commands->SetViewport(0, 0, kWidth, kHeight);
                commands->SetScissor(0, 0, kWidth, kHeight);
                commands->Draw(3);
                commands->EndRenderPass();
                commands->Barrier(ResourceBarrier::CreateTextureBarrier(depth, ResourceState::DepthWrite, ResourceState::CopySource));
                commands->CopyTextureToBuffer(depth, readback, kWidth, kHeight);
                commands->End();
                device->ExecuteCommandLists({commands.get()});
                device->WaitForIdle();
                const auto* values = static_cast<const float*>(device->MapBuffer(readback));
                ASSERT_NE(values, nullptr);
                for (size_t band = 0; band < depthCase.Expected.size(); ++band)
                {
                    // A diagnostic can retain the empty sample in color. The coverage passes reject it.
                    const bool deferredHole = depthCase.DeferColorCoverage && !passDefine && band == 2;
                    const float expected = deferredHole ? 0.5f : depthCase.Expected[band];
                    EXPECT_NEAR(values[band * 16 + 8], std::max(underlyingDepth, expected), 1e-6f);
                }
                device->UnmapBuffer(readback);
            }
            device->DestroyBuffer(readback);
            device->DestroyTexture(depth);
            device->DestroyPipeline(pipeline);
        }
    }
    device->Shutdown();
    std::error_code error;
    std::filesystem::remove_all(cache, error);
}
