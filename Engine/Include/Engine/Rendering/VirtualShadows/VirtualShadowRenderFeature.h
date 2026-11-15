#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowOptions.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/FlatMap.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{
struct DepthProducerState;
struct FeatureDeclareContext;
class RenderServices;
struct ShadowCasterChangeSet;
struct VirtualShadowFeedbackLayout;
struct VirtualShadowPageWork;

/// Owns per-view virtual depth residency and submission certificates.
/// Declaration, preparation, submission and view release are serialized by the renderer.
/// The renderer drains graph streams before destroying the feature.
class VirtualShadowRenderFeature final : public IRenderFeature
{
  public:
    struct RasterPlan
    {
        // Work belongs to this graph incarnation, not the next declaration's view state.
        std::span<const VirtualShadowPageWork> Work;
        Rendering::RenderGraph::RGTexture Atlas;
        Rendering::TextureHandle PhysicalAtlas;
        uint64_t ViewGeneration = 0;
        uint64_t AtlasGeneration = 0;
        Rendering::RenderGraph::RGFrameStamp For;
    };

    VirtualShadowRenderFeature();
    ~VirtualShadowRenderFeature() override;

    void Initialize(Rendering::IDevice* device);
    bool Configure(const VirtualShadowOptions& options);
    bool IsEnabled() const;
    const VirtualShadowOptions& GetOptions() const { return m_Options; }
    void OnScheduleCulling(const FeatureCullingContext& context) override;

    RasterPlan DeclareFeedback(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId view,
        Rendering::RenderGraph::RGBuffer requests, const bool* marked,
        const VirtualShadowFeedbackLayout& layout, const ShadowCasterChangeSet& casters,
        uint64_t geometryRevision, const char* passName);

    /// Return the device-local table after current producer proofs and page raster declarations.
    /// Lighting must declare a storage read of this table and a sampled read of the atlas.
    Rendering::RenderGraph::RGBuffer DeclarePages(Rendering::RenderGraph::RGFrame& frame,
        RenderServices& services, const FeatureDeclareContext& context, const RasterPlan& plan,
        const VirtualShadowFeedbackLayout& layout, std::span<const Rendering::CameraData> cameras);

    /// Called by the table's single preparation task before publishing a resident mapping.
    bool ValidatePage(Rendering::ViewId view, const RasterPlan& plan,
                      const VirtualShadowPageWork& work, const DepthProducerState& producer);

    /// Called only after this page's complete prepared draw packet records and submits.
    bool CertifyPage(Rendering::ViewId view, const RasterPlan& plan,
                     const VirtualShadowPageWork& work, const DepthProducerState& producer,
                     bool recorded);

    void OnViewReleased(Rendering::ViewId view) override;
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameAbortedRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

  private:
    struct ViewState;
    struct PreparedPage;

    ViewState* FindView(Rendering::ViewId view, const RasterPlan& plan);
    bool IsCurrentPage(Rendering::ViewId view, const RasterPlan& plan, const VirtualShadowPageWork& work);
    bool MatchesLayout(Rendering::ViewId view, const RasterPlan& plan,
                        const VirtualShadowFeedbackLayout& layout);
    bool PreparePageTable(Rendering::RenderGraph::RGPrepareContext& context, const RasterPlan& plan,
        Rendering::ViewId view, std::span<PreparedPage> pages, std::span<uint32_t> words, RenderServices& services);
    void CollectRetiredViews();

    Rendering::IDevice* m_Device = nullptr;
    VirtualShadowOptions m_Options;
    uint64_t m_NextViewGeneration = 1;
    FlatMap<Rendering::ViewId, std::unique_ptr<ViewState>> m_Views;
    std::vector<std::unique_ptr<ViewState>> m_RetiredViews;
};
} // namespace GameEngine::Engine::Renderer
