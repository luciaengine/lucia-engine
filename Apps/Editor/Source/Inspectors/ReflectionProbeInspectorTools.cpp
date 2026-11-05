#include "Inspectors/ReflectionProbeInspectorTools.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/TextArea.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

namespace GameEngine {
namespace {
using Ibl = Engine::Renderer::ImageBasedLightingFeature;
using ProbeSource = Engine::Renderer::SceneReflectionProbeEnvironmentSource;
Ibl* FindIbl()
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized()) return nullptr;
    auto* services = engine.GetRenderServices();
    return services ? services->GetFeature<Ibl>() : nullptr;
}
struct ProbeToolsState
{
    UIElement* Host = nullptr;
    Foldout* Details = nullptr;
    TextArea* Status = nullptr;
    TextArea* Weights = nullptr;
    uint64_t WorldId = 0, SourceKey = 0;
    float Sample[3]{};
    uint32_t RefreshCounter = 0;
};
class ProbeToolsHost final : public UIElement
{
public:
    std::shared_ptr<ProbeToolsState> State;
    ~ProbeToolsHost() override
    {
        if (!State) return;
        State->Host = nullptr;
    }
};
TextArea* AddReadout(UIElement* parent)
{
    auto text = std::make_unique<TextArea>();
    text->SetReadOnly(true);
    text->AddClass("inspector-text");
    auto* raw = text.get();
    parent->AddChild(std::move(text));
    return raw;
}
void AddAction(UIElement* parent, const char* text, std::function<void()> action)
{
    auto button = std::make_unique<Button>();
    button->SetText(text);
    button->AddClass("inspector-button");
    button->RegisterEventHandler(kEventButtonClick, [action = std::move(action)](UIEvent&) { action(); });
    parent->AddChild(std::move(button));
}
std::array<float, 3> ProbeWeights(const Ibl& ibl, uint64_t worldId, const float position[3])
{
    std::array<float, 3> weights{};
    uint32_t priorities[2]{};
    for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
    {
        const auto* capture = ibl.GetLocalProbeCapture(worldId, i);
        const auto* p = ibl.GetLocalProbeDesc(worldId, i);
        if (!capture || !p || !capture->IsInitialized() || capture->GetLastBakedDigest() == 0) continue;
        priorities[i] = p->Priority;
        float local[3]{};
        const float* axes[] = {p->BoxAxisX, p->BoxAxisY, p->BoxAxisZ};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) local[a] += (position[b] - p->BoxCenter[b]) * axes[a][b];
        const float extent = std::min({p->BoxHalfExtents[0], p->BoxHalfExtents[1], p->BoxHalfExtents[2]});
        if (extent <= 1e-4f) continue;
        float edge;
        const bool box = p->InfluenceShape == 1;
        if (box) edge = std::min({p->BoxHalfExtents[0] - std::abs(local[0]),
            p->BoxHalfExtents[1] - std::abs(local[1]), p->BoxHalfExtents[2] - std::abs(local[2])});
        else
        {
            float radius2 = 0, length2 = 0;
            for (int a = 0; a < 3; ++a) { radius2 += local[a] * local[a] / (p->BoxHalfExtents[a] * p->BoxHalfExtents[a]); length2 += local[a] * local[a]; }
            const float radius = std::sqrt(radius2);
            if (radius > 1) continue;
            edge = radius > 1e-4f ? std::sqrt(length2) * (1 / radius - 1) : extent;
        }
        if (edge < 0) continue;
        const float blend = std::min(p->BlendDistance, extent);
        weights[i] = blend > 1e-4f ? std::clamp(edge / blend, 0.0f, 1.0f) : 1.0f;
    }
    if (priorities[0] > priorities[1]) weights[1] *= 1 - weights[0];
    else if (priorities[1] > priorities[0]) weights[0] *= 1 - weights[1];
    const float total = weights[0] + weights[1];
    weights[2] = std::max(0.0f, 1 - total);
    for (auto& w : weights) w /= std::max(1.0f, total);
    return weights;
}


void RefreshTools(const std::shared_ptr<ProbeToolsState>& state)
{
    if (!state->Host) return;
    auto* ibl = FindIbl();
    if (!ibl) return;
    const bool expanded = state->Details->IsExpanded();
    if (!expanded || (++state->RefreshCounter % 12) != 1) return;
    const auto weights = ProbeWeights(*ibl, state->WorldId, state->Sample);
    char text[1024]{};
    std::string status;
    bool selectedResident = false;
    for (uint32_t slot = 0; slot < Ibl::kMaxLocalProbes; ++slot)
    {
        const auto* capture = ibl->GetLocalProbeCapture(state->WorldId, slot);
        const auto* desc = ibl->GetLocalProbeDesc(state->WorldId, slot);
        if (!capture || !desc) continue;
        const auto* source = static_cast<const ProbeSource*>(capture->GetEnvironmentSource());
        const auto d = source->GetDiagnostics();
        selectedResident |= desc->SourceKey == state->SourceKey;
        std::snprintf(text, sizeof(text),
            "%s: entity %llu%s\n%s %u | %s | age %.1f s | bakes %llu\nCPU capture prepare %.2f ms; filter record %.2f ms\n",
            slot == 0 ? "Orange" : "Blue", static_cast<unsigned long long>(desc->SourceKey & 0xffffffffu),
            desc->SourceKey == state->SourceKey ? " (selected)" : "",
            d.Stage, d.Step, capture->GetLastBakedDigest() ? "Ready" : "Sky fallback",
            d.AgeSeconds, static_cast<unsigned long long>(d.CompletedBakes), d.CaptureRecordMs, d.FilterRecordMs);
        status += text;
        if (d.LastCaptureGpuSpanMs >= 0 || d.LastFilterGpuSpanMs >= 0)
        {
            std::snprintf(text, sizeof(text), "Last GPU spans: face %.3f ms; filter %.3f ms%s\n",
                d.LastCaptureGpuSpanMs, d.LastFilterGpuSpanMs, d.GpuSpansShared ? " (shared encoder upper bounds)" : "");
            status += text;
        }
    }
    if (!selectedResident) status += "Selected probe is not retained. This tier keeps two enabled probes by priority.\n";
    status += "GPU spans appear with Render Graph profiling enabled; -1 means unavailable.";
    state->Status->SetValue(status);
    std::snprintf(text, sizeof(text), "At sample point: orange %.1f%% | blue %.1f%% | sky %.1f%%",
        weights[0] * 100, weights[1] * 100, weights[2] * 100);
    state->Weights->SetValue(text);
}
void AddReflectionProbeInspectorTools(const InspectorContext& ctx)
{
    auto host = std::make_unique<ProbeToolsHost>();
    host->AddClass("inspector-section");
    auto state = std::make_shared<ProbeToolsState>();
    state->Host = host.get();
    state->WorldId = ctx.World->GetWorldId();
    state->SourceKey = (state->WorldId << 32u) ^ static_cast<uint64_t>(ctx.Entity.id);
    if (state->SourceKey == 0) state->SourceKey = 1;
    host->State = state;
    if (auto* ibl = FindIbl())
        for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
            if (const auto* desc = ibl->GetLocalProbeDesc(state->WorldId, i); desc && desc->SourceKey == state->SourceKey)
                for (int a = 0; a < 3; ++a) state->Sample[a] = desc->Position[a];
    std::vector<uint64_t> selected;
    for (auto entity : ctx.Entities) selected.push_back((state->WorldId << 32u) ^ static_cast<uint64_t>(entity.id));
    if (selected.empty()) selected.push_back(state->SourceKey);
    AddAction(host.get(), "Rebake Selected Probes", [state, selected] {
        if (auto* ibl = FindIbl()) for (auto key : selected) ibl->RequestLocalProbeRebake(state->WorldId, key);
    });
    AddAction(host.get(), "Rebake All Probes in Scene", [state] {
        if (auto* ibl = FindIbl()) ibl->RequestLocalProbeRebake(state->WorldId);
    });
    auto details = std::make_unique<Foldout>();
    details->SetTitle("Reflection Diagnostics");
    details->SetExpanded(false);
    state->Details = details.get();
    auto* content = details.get();
    state->Status = AddReadout(content);
    const char* axes[] = {"Sample X", "Sample Y", "Sample Z"};
    for (int axis = 0; axis < 3; ++axis)
    {
        auto update = [state, axis](float value) { state->Sample[axis] = value; };
        InspectorDrag::AddFloatRowWithDrag(content, axes[axis], state->Sample[axis], update, update);
    }
    state->Weights = AddReadout(content);
    host->AddChild(std::move(details));
    ctx.Parent->AddChild(std::move(host));
    if (ctx.FrameRefreshCallbacks)
        ctx.FrameRefreshCallbacks->push_back([weak = std::weak_ptr<ProbeToolsState>(state)] {
            if (auto state = weak.lock()) RefreshTools(state);
        });
}

void RenderReflectionProbeInspector(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid() ||
        !ctx.World->GetComponent<Components::ReflectionProbe>(ctx.Entity))
        return;
    AddReflectionProbeInspectorTools(ctx);
    RenderDefaultComponentInspector(ctx, ECS::GetComponentTypeId<Components::ReflectionProbe>());
}
} // namespace

void RegisterReflectionProbeInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<Components::ReflectionProbe>(RenderReflectionProbeInspector);
}

} // namespace GameEngine
