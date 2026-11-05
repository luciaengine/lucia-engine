#pragma once

// Renders a material graph's node plates and main preview into one persistent atlas.
// Each cell draws a unit sphere with its preview material. Each panel owns an atlas
// with unique GPU and UI resource names.

#include "AssetCore/GUID.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"
#include "ShaderGraph/GraphPreviewAtlasLayout.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Handle.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class UIManager;

namespace Rendering::RenderGraph
{
class RGFrame;
struct RGTexture;
}

namespace Engine::Renderer
{
class RenderServices;
}

namespace Editor
{

class GraphPreviewAtlas
{
  public:
    GraphPreviewAtlas() = default;
    ~GraphPreviewAtlas();
    GraphPreviewAtlas(const GraphPreviewAtlas&) = delete;
    GraphPreviewAtlas& operator=(const GraphPreviewAtlas&) = delete;

    /// Silhouette fraction of a cell: full-bleed for node plates, framed for the
    /// graph's own preview panel.
    static constexpr float kNodeCellFill = 0.88f;
    static constexpr float kMainPreviewFill = 0.70f;

    struct NodeRequest
    {
        std::string NodeId;
        GUID MaterialGuid{};
        /// Rotation about Y applied to this cell's sphere. Node cells leave it
        /// at zero; the graph's main preview spins and drags on it.
        float YawRadians = 0.f;
        /// Fraction of the cell the sphere spans. Node plates are full-bleed by
        /// design; the graph's preview panel is a framed picture and wants the
        /// margin the thumbnail slot used to give it.
        float FillFraction = kNodeCellFill;
    };

    /// Binds services on the editor frame thread; they must stay stable through Execute.
    void Initialize(Engine::Renderer::RenderServices* services);
    /// Invalidates queued plans and releases this atlas on the editor frame thread.
    void Shutdown();

    /// The node previews the graph panel wants drawn, in graph order. Replaces
    /// the previous set; cells of nodes that disappeared are recycled. Returns
    /// true when the set actually moved, which is the caller's cue to rebind.
    bool SetRequests(uint64_t windowId, std::vector<NodeRequest> requests);

    /// Drops every request and unbinds the atlas from the UI. Called when the
    /// material graph closes or collapses its expanded nodes. Returns true when
    /// requests were actually released, which is the caller's cue to rebind.
    bool ClearRequests();

    /** A rebuild rebinds the whole canvas, which destroys the element under the
        pointer and kills an in-flight drag — so when a gesture owns the mouse
        the caller defers it here and the atlas replays it on release.
        Capped: a wedged gesture must never leave stale plates on screen
        forever, so past the cap the rebuild runs even at the cost of the drag.
        The host only reports whether a gesture is live; the atlas owns the
        policy, because the atlas is what rebuilds. */
    void DeferRebuild() { m_RebuildDeferred = true; }
    /** Consumes a pending deferral and returns true when the caller should
        rebuild now — either the gesture ended or the cap expired. Returns false
        (and keeps the deferral) while a gesture is still live. "Take" because
        it clears the flag: call it once per frame and act on the result. */
    bool TakeDeferredRebuild(bool gestureActive);

    /// Requests a redraw and invalidates previously declared plans on the editor frame thread.
    void MarkDirty();

    /// Spins one cell's sphere. Redraws only when the angle actually moves, so
    /// a still preview leaves the pass out of the frame.
    void SetRequestYaw(const std::string& nodeId, float yawRadians);

    bool TryGetBinding(const std::string& nodeId, GraphNodePreviewBinding& out) const;

    /// Declares atlas rendering and UI reads on the editor frame thread.
    /// RenderServices and its device must remain stable through the frame's Execute.
    void TickRG(uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame);

    /** Renders cells with each material's IBL keyword variant (environment
        cubes + BRDF LUT bound like the world pass) instead of the base
        single-light pipeline. Governs the atlas cells only; the graph's main
        preview sphere carries its own flag. */
    void SetIblEnabled(bool enabled);

  private:
    struct AtlasGeneration;
    struct Completion;
    struct State;
    struct FramePlan;

    struct CellDraw
    {
        uint32_t Cell = 0;
        GUID MaterialGuid{};
        uint64_t Registration = 0;
        uint32_t Version = 0;
        uint32_t ContentRevision = 0;
        float YawRadians = 0.f;
        float FillFraction = kNodeCellFill;
    };

    // Desired content is compared against the last recorded atlas completion.
    uint64_t ComputeContentDigest() const;
    void DrainCompletion();
    void PublishToUi(UIManager* ui, Rendering::RenderGraph::RGFrame& frame,
                     Rendering::RenderGraph::RGTexture atlas, bool hasPlannedProducer);
    void EnsureView();
    bool EnsureAtlasTexture(uint32_t width, uint32_t height);
    void SubmitPreviewLights();
    Rendering::CameraData BuildCameraData() const;
    /// IBL cells only draw when the feature exists AND has finished building its
    /// cubes; without that gate the keyword is recorded, every draw is skipped,
    /// and the undrawn-retry re-records the pass every frame forever.
    bool IblAvailable() const;

    Engine::Renderer::RenderServices* m_Services = nullptr;
    std::weak_ptr<std::atomic<bool>> m_ServiceLifetime;
    std::shared_ptr<State> m_State;
    std::shared_ptr<AtlasGeneration> m_Generation;

    GraphPreviewAtlasLayout m_Layout;
    std::vector<NodeRequest> m_Requests;
    uint64_t m_WindowId = 0;

    // Per-instance names: the UI texture the thumbs bind and the render-graph
    // resource keys. ImportPersistentTexture is keyed by name, so sharing these
    // across panels would alias two atlases of different sizes onto one texture.
    std::string m_UiResourceName;
    std::string m_AtlasResourceKey;
    std::string m_MsaaResourceKey;
    std::string m_DepthResourceKey;
    std::string m_HdrResourceKey;
    std::string m_PassName;
    std::string m_TonemapPassName;
    // Light world for this atlas's view. Never an ECS world id, and never
    // shared: two panels submitting into one world would sum their key lights.
    uint64_t m_LightWorldId = 0;

    Rendering::TextureHandle m_AtlasTexture{};
    uint32_t m_AtlasWidth = 0;
    uint32_t m_AtlasHeight = 0;
    bool m_AtlasInitialized = false;
    bool m_UiBound = false;

    Rendering::CameraId m_CameraId = 0;
    Rendering::ViewId m_ViewId = 0;

    bool m_Dirty = true;

    bool m_IblEnabled = false;
    bool m_RebuildDeferred = false;
    int m_RebuildDeferredFrames = 0;
    static constexpr int kMaxDeferredRebuildFrames = 90;

    // Missing or older shader versions keep the atlas pending until current.
    uint32_t m_UndrawnCells = 0;
    uint64_t m_ContentDigest = 0;
};

} // namespace Editor
} // namespace GameEngine
