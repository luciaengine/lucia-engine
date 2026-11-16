#include "DebugServer/ReflectionProbeDebugHandlers.h"

#include "DebugServer/EditorDebugServer.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine
{

using json = nlohmann::json;
using Ibl = Engine::Renderer::ImageBasedLightingFeature;
using ProbeSource = Engine::Renderer::SceneReflectionProbeEnvironmentSource;

namespace
{

// The low 32 bits of a probe's source key are its entity id.
constexpr uint64 kEntityIdMask = 0xffffffffu;

json Position(const Mathematics::WorldPosition& position)
{
    return json::array({position.X, position.Y, position.Z});
}

json DescribeProbe(uint32_t slot, const Ibl& capture,
                   const Engine::Renderer::SceneReflectionProbeEnvironmentDesc& desc)
{
    const auto* source = static_cast<const ProbeSource*>(capture.GetEnvironmentSource());
    const ProbeSource::Diagnostics diagnostics = source->GetDiagnostics();
    return json{
        {"slot", slot},
        {"entityId", desc.SourceKey & kEntityIdMask},
        {"sourceKey", std::to_string(desc.SourceKey)},
        {"priority", desc.Priority},
        {"updateMode", static_cast<uint32_t>(desc.UpdateMode)},
        {"ready", capture.IsInitialized() && capture.GetLastBakedDigest() != 0},
        {"baking", source->IsBaking()},
        {"needsRefresh", source->NeedsBake()},
        {"stage", diagnostics.Stage},
        {"step", diagnostics.Step},
        {"completedBakes", diagnostics.CompletedBakes},
        {"ageSeconds", diagnostics.AgeSeconds},
        {"waitSeconds", diagnostics.WaitSeconds},
        {"capturePrepareCpuMs", diagnostics.CaptureRecordMs},
        {"filterRecordCpuMs", diagnostics.FilterRecordMs},
        {"lastFaceGpuSpanMs", diagnostics.LastCaptureGpuSpanMs},
        {"lastFilterGpuSpanMs", diagnostics.LastFilterGpuSpanMs},
        {"gpuSpansShared", diagnostics.GpuSpansShared},
        {"sceneEditDigest", std::to_string(desc.SceneEditDigest)},
        {"inputDigest", std::to_string(source->InputDigest())},
        {"bakedDigest", std::to_string(capture.GetLastBakedDigest())},
        {"captureResolution", capture.GetEnvCaptureSize()},
        {"prefilterMaxMip", capture.GetPrefilterMaxMip()},
        {"capturePosition", Position(source->GetCapturedPosition())},
        {"requestedPosition", Position(desc.Position)},
        {"influenceShape", desc.InfluenceShape},
        {"boxProjection", desc.BoxProjection}};
}

json ReflectionProbeState(const EditorDebugServer::RequestContext&)
{
    auto& engine = EngineCore::GetInstance();
    const ECS::World* world = engine.GetPrimaryWorld();
    Engine::Renderer::RenderServices* services = engine.GetRenderServices();
    const Ibl* ibl = services ? services->GetFeature<Ibl>() : nullptr;
    return DescribeReflectionProbes(world ? ibl : nullptr, world ? world->GetWorldId() : 0u);
}

json RebakeReflectionProbes(const EditorDebugServer::RequestContext& context)
{
    auto& engine = EngineCore::GetInstance();
    const ECS::World* world = engine.GetPrimaryWorld();
    Engine::Renderer::RenderServices* services = engine.GetRenderServices();
    Ibl* ibl = services ? services->GetFeature<Ibl>() : nullptr;
    if (!world || !ibl)
        return json{{"queued", 0}};
    const uint64 entityId = context.params.value("entityId", uint64{0});
    uint32_t queued = 0;
    for (uint32_t slot = 0; slot < Ibl::kMaxLocalProbes; ++slot)
    {
        const auto* desc = ibl->GetLocalProbeDesc(world->GetWorldId(), slot);
        if (!desc || (entityId != 0 && (desc->SourceKey & kEntityIdMask) != entityId))
            continue;
        ibl->RequestLocalProbeRebake(world->GetWorldId(), desc->SourceKey);
        ++queued;
    }
    return json{{"queued", queued}};
}

} // namespace

json DescribeReflectionProbes(const Ibl* ibl, uint64 worldId)
{
    json probes = json::array();
    if (ibl)
    {
        for (uint32_t slot = 0; slot < Ibl::kMaxLocalProbes; ++slot)
        {
            const auto* capture = ibl->GetLocalProbeCapture(worldId, slot);
            const auto* desc = ibl->GetLocalProbeDesc(worldId, slot);
            if (capture && desc)
                probes.push_back(DescribeProbe(slot, *capture, *desc));
        }
    }
    return json{{"worldId", worldId},
                {"capacity", Ibl::kMaxLocalProbes},
                {"workUnit", "one capture face or one filtering dispatch"},
                {"probes", std::move(probes)}};
}

void RegisterReflectionProbeDebugHandlers(EditorDebugServer& server)
{
    server.RegisterHandler("get_reflection_probe_state", ReflectionProbeState);
    server.RegisterHandler("rebake_reflection_probes", RebakeReflectionProbes);
}

} // namespace GameEngine
