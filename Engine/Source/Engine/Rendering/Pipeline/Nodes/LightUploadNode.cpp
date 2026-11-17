#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"

#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Core/Time.h"

#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
static_assert(sizeof(LightUploadNode::GPULightPacked) == 144, "GPULightPacked must be 144 bytes");

bool LightUploadNode::IsBufferedLight(const ExtractedLight& light)
{
    return light.type != GameEngine::Components::LightType::Directional &&
           light.type != GameEngine::Components::LightType::Ambient;
}

LightUploadNode::GPULightPacked LightUploadNode::PackLight(const ExtractedLight& in, const CameraData& camera,
                                                           int32_t shadowSlot)
{
    GPULightPacked p{};
    p.type = static_cast<uint32_t>(in.type);
    p.castsShadows = in.castsShadows;
    p.areaShape = static_cast<uint32_t>(in.areaShape);
    p.castsLight = in.castsLight;
    const float inner = std::clamp(in.innerAngle, 0.0f, 3.1415926f);
    const float outer = std::clamp(in.outerAngle, 0.0f, 3.1415926f);
    p.spotCosInner = std::cos(std::min(inner, outer));
    p.spotCosOuter = std::cos(std::max(inner, outer));
    const Mathematics::Vector3 position = ViewRelativePoint(camera, in.positionWS);
    p.position[0] = position.x;
    p.position[1] = position.y;
    p.position[2] = position.z;
    p.range = in.range;
    p.directionWS[0] = in.directionWS[0];
    p.directionWS[1] = in.directionWS[1];
    p.directionWS[2] = in.directionWS[2];
    p.areaRightWS[0] = in.rightWS[0];
    p.areaRightWS[1] = in.rightWS[1];
    p.areaRightWS[2] = in.rightWS[2];
    p.areaUpWS[0] = in.upWS[0];
    p.areaUpWS[1] = in.upWS[1];
    p.areaUpWS[2] = in.upWS[2];
    p.intensity = in.intensity;
    p.color[0] = in.color[0];
    p.color[1] = in.color[1];
    p.color[2] = in.color[2];
    p.areaWidth = std::max(in.areaWidth, 0.001f);
    p.areaHeight = std::max(in.areaHeight, 0.001f);
    p.areaRadius = std::max(in.areaRadius, 0.001f);
    p.falloffMode = static_cast<float>(static_cast<uint32_t>(in.falloff));
    p.decay = std::max(in.decay, 0.0f);
    p.fogContribution = std::max(in.fogContribution, 0.0f);
    p.fogDensityBoost = std::max(in.fogDensityBoost, 0.0f);
    p.fogOriginFade = Math::Clamp01(in.fogOriginFade);
    p.fogAnisotropy = std::clamp(in.fogAnisotropy, -0.95f, 0.95f);
    p.shadowSlot = shadowSlot;
    return p;
}

bool LightUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    m_ForwardBufferRef.clear();

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object())
        {
            if (j.contains("forwardBuffer") && j["forwardBuffer"].is_string())
                m_ForwardBufferRef = j["forwardBuffer"].get<std::string>();
            if (j.contains("buffer") && j["buffer"].is_string())
            {
                m_BufferRef = j["buffer"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void LightUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring, EXACT-size: header +
    // count*sizeof(GPULightPacked) instead of the blueprint's full kMaxLights
    // worst case — every byte written at
    // declaration, so the old zero-fill of unused records has nothing to
    // cover. Directional/ambient stay out-of-band via LightUBO (same filter
    // as the old exec).
    const auto* viewDesc = d.Services.Views().FindViewDesc(d.View.id);
    const uint64_t worldId = viewDesc ? viewDesc->worldId : 0u;
    // Pre-sorted strongest-first by RenderServices::FinalizeWorldLights, so the
    // kMaxLights truncation below (a first-N walk) keeps the top-contribution
    // lights deterministically instead of dropping by arbitrary iteration order.
    const auto lights = d.Services.GetWorldLights(worldId);

    if (!m_ForwardBufferRef.empty())
    {
        auto forward = d.Frame.AllocUpload<ForwardLightUBO>();
        if (forward.Ptr)
        {
            ForwardLightUBO ubo{};
            ubo.uLightVP[0] = ubo.uLightVP[5] = ubo.uLightVP[10] = ubo.uLightVP[15] = 1.0f;
            PackForwardLightDirectionals(lights, ubo);
            for (const auto& light : lights)
            {
                if (light.type != GameEngine::Components::LightType::Ambient || !light.castsLight)
                    continue;
                for (size_t i = 0; i < 3; ++i)
                    ubo.uAmbient[i] += light.color[i] * light.intensity;
            }
            ubo.uAmbient[3] = 1.0f;
            ubo.uTimeParams[0] = d.Services.GetShaderAnimationTimeSeconds();
            ubo.uTimeParams[1] = d.Services.GetScrollAnimationTimeSeconds();
            ubo.uTimeParams[2] =
                d.Services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds()).TimeSeconds;
            ubo.uTimeParams[3] = ubo.uTimeParams[1];
            d.Services.PackViewWeather(d.View.id, worldId, ubo);
            std::memcpy(forward.Ptr, &ubo, sizeof(ubo));
            d.PublishBuffer(m_ForwardBufferRef, {forward.Buffer, forward.Offset, sizeof(ubo), {}});
        }
    }

    constexpr uint32_t kMaxLights = 1024;

    uint32_t clusterableCount = 0;
    for (const auto& in : lights)
    {
        if (!IsBufferedLight(in))
            continue;
        if (clusterableCount >= kMaxLights)
            break;
        ++clusterableCount;
    }

    // At least one (zeroed) record even with no clusterable lights: WebGPU
    // validates the binding against the WGSL struct's minimum size, which
    // counts one element of the runtime light array; the header's count=0
    // keeps the shader from reading it.
    const size_t bytes =
        kLightBufferHeaderBytes + size_t(std::max(clusterableCount, 1u)) * sizeof(GPULightPacked);
    const auto alloc = d.Frame.AllocUpload(bytes, 256);
    if (!alloc.Ptr)
        return;

    uint8_t* dst = static_cast<uint8_t*>(alloc.Ptr);
    std::memset(dst, 0, bytes);
    std::memcpy(dst, &clusterableCount, sizeof(uint32_t));

    // M1: per-light point-shadow atlas slot, indexed by the same packed clusterable
    // index this loop assigns (writeIdx). Computed once per (view, frame) by the
    // shadow assignment; -1 for lights the atlas budget did not admit.
    const auto camData = d.Services.Views().ResolveCameraData(d.View.id);
    const std::span<const int32_t> shadowSlotOfCluster =
        d.Services.PointShadowSlotOfCluster(d.View.id, worldId, camData);

    auto* outLights = reinterpret_cast<GPULightPacked*>(dst + kLightBufferHeaderBytes);
    uint32_t writeIdx = 0;
    for (const auto& in : lights)
    {
        if (!IsBufferedLight(in))
            continue;
        if (writeIdx >= kMaxLights)
            break;
        const int32_t shadowSlot = writeIdx < shadowSlotOfCluster.size() ? shadowSlotOfCluster[writeIdx] : -1;
        outLights[writeIdx] = PackLight(in, camData, shadowSlot);
        ++writeIdx;
    }

    d.PublishBuffer(m_BufferRef, {alloc.Buffer, alloc.Offset, bytes, /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
