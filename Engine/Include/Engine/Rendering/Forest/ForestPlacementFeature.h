#pragma once

#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"

#include <memory>
#include <optional>

namespace GameEngine { class AssetManager; }

namespace GameEngine::Engine::Renderer
{

struct ForwardEmitContext;
struct ForestPlacementSession;
struct ImpostorBakePart;
struct CascadeFrameData;

struct ForestCascadeDraws
{
    std::vector<DrawCommand> Meshes;
    std::vector<Rendering::RenderGraph::RGBuffer> Transforms;
};

struct ForestSunShadows
{
    [[nodiscard]] const ForestCascadeDraws* DrawsForCascade(uint32 index) const
    {
        if (index >= Cascades.size() || Cascades[index].Meshes.empty())
            return nullptr;
        return &Cascades[index];
    }

    std::array<ForestCascadeDraws, 4> Cascades;
};

struct ForestImpostorCard
{
    GUID Material;
    Mathematics::Vector4 Bounds;
    ImpostorFoliageSway Sway;
    float SwitchCoverage = 0.0f;
};

/// Owns placement sessions through render graph submission and device recovery.
class ForestPlacementFeature final : public IRenderFeature
{
public:
    /// The scene's stand producer owns the returned session.
    [[nodiscard]] std::shared_ptr<ForestPlacementSession> Attach(std::shared_ptr<ForestPlacementRuntime> placements,
        AssetManager& assets, uint64 world, const GUID& model, float detailDistance, uint32 nearBudget = 0);

    bool HasSunShadowSource(const RenderServices& services, const Rendering::ViewDesc& view) const;
    static bool SunShadowCardsEnabled();
    void PublishSunShadowCard(uint64 world, const GUID& model, const GUID& material);
    bool WantsAtlas(uint64 world, const GUID& model) const;
    bool ConsumeShadowSourceDemand(uint64 world, const GUID& model);
    void PublishShadowSources(uint64 world, const GUID& model, const Mathematics::Vector4& bounds,
                              std::span<const ImpostorBakePart> sources);
    void ClearCards(uint64 world);
    void PublishCard(uint64 world, const GUID& model, const ForestImpostorCard& card,
                     std::span<const ImpostorBakePart> sources = {});
    /// Return true when standRanges accounts for all visible cells not drawn by the packed stream.
    bool EmitDetails(ForestPlacementSession& session, const ForwardEmitContext& context,
                     const Rendering::CameraData& camera, ForestPlacementView view,
                     std::vector<DrawSubCommand>& standRanges);

    const ForestSunShadows* PrepareSunShadows(Rendering::RenderGraph::RGFrame& frame,
        RenderServices& services, Rendering::ViewId viewId, const CascadeFrameData& fit);

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameAbortedRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    void Finish(Rendering::RenderGraph::RGFrame& frame, bool submitted);
    std::vector<std::weak_ptr<ForestPlacementSession>> m_Sessions;
    uint64 m_NextSession = 0;
};

} // namespace GameEngine::Engine::Renderer
