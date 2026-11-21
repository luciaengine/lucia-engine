#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Rendering/Core/Device.h"
#include "Types/FlatMap.h"

#include <array>
#include <deque>
#include <mutex>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{
struct RGTexture;
}

namespace GameEngine::Engine::Renderer
{

class RenderServices;
struct DrawCommand;

/// One mesh of the object an atlas shows, with the material it bakes with: a material without
/// a vertex modifier, so the atlas holds the rest pose.
struct ImpostorBakePart
{
    Rendering::MeshGPUHandle Mesh;
    GUID Material;
};

/// What to bake when an atlas is absent.
struct ImpostorBakeRequest
{
    ImpostorAtlasKey Key;
    // The sphere the frames fit, object space, as the parts' vertices: the tightest one around
    // the vertices gives the sharpest frames.
    Mathematics::Vector3 Center;
    float Radius = 1.0f;
    std::vector<ImpostorBakePart> Parts;
};

/// A baked atlas: FramesPerSide x FramesPerSide tiles of FrameSize pixels.
/// Color: RGB base colour, A coverage. NormalDepth: RGB object normal * 0.5 + 0.5, A the depth along
/// the frame axis (1 at the bounding sphere's near side, 0 at its far side).
/// SeasonMask: R leaf tint weight, G snow receptivity.
/// DiffuseTransmission: RGB albedo multiplied by transmission share, A the share. Optional.
struct ImpostorAtlas
{
    Rendering::TextureHandle Color;
    Rendering::TextureHandle NormalDepth;
    Rendering::TextureHandle SeasonMask;
    Rendering::TextureHandle DiffuseTransmission;
    uint32 HighestReadyMip = 0;
};

/// Bakes impostor atlases within a byte budget. Each submitted frame advances one pending bake.
/// A bake whose source pipelines are still building yields to the next job.
/// The bake view runs independently of the render pipeline, culling and extraction.
class ImpostorBaker final : public IRenderFeature
{
public:
    // Atlas frames drawn per rendered frame: 64 frames bake in 16 frames.
    static constexpr uint32 kFramesPerRenderedFrame = 4;

    explicit ImpostorBaker(uint64 budgetBytes);
    ~ImpostorBaker() override;

    /// The atlas of `request.Key` once it is baked; an atlas with invalid textures until then.
    /// The first call for a key queues its bake when it fits the budget. Thread safe.
    ImpostorAtlas Request(const ImpostorBakeRequest& request, uint64 frame);

    void DeclareFrameStage(Rendering::RenderGraph::RGFrame& frame, RenderServices& services) override;
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameAbortedRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    struct BakeJob
    {
        ImpostorBakeRequest Request;
        std::vector<uint64> SourceRevisions{};
        uint32 NextFrame = 0;     // The next atlas frame to draw.
        uint32 SettledFrames = 0; // Frames in a row with no pipeline build in flight for its materials.
        bool Cleared = false;
        uint32 NextMip = 1;
    };
    struct PendingBake
    {
        const Rendering::RenderGraph::RGFrame* Frame = nullptr;
        uint64 FrameIndex = 0;
        std::array<const bool*, kFramesPerRenderedFrame> RecordedTiles{};
        bool Yield = false;
        uint32 RecordedMip = 0;
        std::array<const bool*, 4> RecordedMipLayers{};
    };
    struct AtlasTextures
    {
        ImpostorAtlas Atlas;
        std::array<std::vector<Rendering::TextureViewHandle>, 4> SampleViews;
        bool Ready = false;
    };
    // Draw commands and push constants live until the frame that declared them executes.
    static constexpr uint32 kFrameSlots = 4;
    struct FrameSlot
    {
        std::vector<DrawCommand> Commands;
        std::vector<std::array<std::byte, 128>> PushConstants;
    };

    bool EnsureBakeView(RenderServices& services);
    bool EnsureMipPipeline(Rendering::IDevice& device);
    bool DeclareMip(Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
                    AtlasTextures& textures, const BakeJob& job,
                    const std::array<Rendering::RenderGraph::RGTexture, 4>& resources);
    void DestroyAtlas(Rendering::IDevice& device, AtlasTextures& textures);
    AtlasTextures& EnsureTextures(Rendering::IDevice& device, const ImpostorAtlasKey& key);
    void FreeTextures(Rendering::IDevice* device, const ImpostorAtlasKey& key);
    bool PartsSettled(RenderServices& services, BakeJob& job);
    const bool* DeclareAtlasFrame(Rendering::RenderGraph::RGFrame& frame, RenderServices& services, const BakeJob& job,
                           Rendering::RenderGraph::RGTexture color, Rendering::RenderGraph::RGTexture normalDepth,
                           Rendering::RenderGraph::RGTexture seasonMask,
                           Rendering::RenderGraph::RGTexture diffuseTransmission,
                           Rendering::RenderGraph::RGTexture depth,
                           uint32 atlasFrame, FrameSlot& slot, bool warmupOnly);

    std::mutex m_Mutex;
    ImpostorAtlasCache m_Cache;
    FlatMap<ImpostorAtlasKey, AtlasTextures> m_Textures;
    FlatMap<ImpostorAtlasKey, uint64> m_AllocationRetryFrames;
    std::deque<BakeJob> m_Jobs;
    std::vector<ImpostorAtlasKey> m_Evicted;
    PendingBake m_PendingBake;
    std::vector<ImpostorAtlasKey> m_Finished; // Complete on the submitted graphics queue.
    std::array<FrameSlot, kFrameSlots> m_FrameSlots;
    Rendering::IDevice* m_Device = nullptr;
    Rendering::DescriptorSetLayoutDesc m_MipLayout;
    Rendering::GraphicsPipelineId m_MipPipeline{};
    bool m_MipPipelineRequested = false;
    Rendering::CameraId m_Camera{};
    Rendering::ViewId m_View{};
};

} // namespace GameEngine::Engine::Renderer
