#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/Impostors/ImpostorAtlasCache.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Types/FlatMap.h"

#include <array>
#include <deque>
#include <mutex>
#include <optional>
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

/// A baked atlas: FramesPerSide x FramesPerSide tiles of FrameSize pixels in two textures.
/// Color: RGB base colour, A coverage. NormalDepth: RGB object normal * 0.5 + 0.5, A the depth along
/// the frame axis (1 at the bounding sphere's near side, 0 at its far side).
struct ImpostorAtlas
{
    Rendering::TextureHandle Color;
    Rendering::TextureHandle NormalDepth;
    uint32 HighestReadyMip = 0;
};

/// Bakes impostor atlases on the GPU. Each frame it draws a few atlas frames of the oldest
/// pending bake with the ImpostorBake pass keyword, through a view of its own that the render
/// pipeline, culling and extraction skip. The atlases live inside a byte budget.
class ImpostorBaker final : public IRenderFeature
{
public:
    static constexpr uint64 kDefaultBudgetBytes = 256ull << 20;
    // Atlas frames drawn per rendered frame: 64 frames bake in 16 frames.
    static constexpr uint32 kFramesPerRenderedFrame = 4;

    ImpostorBaker();
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
        uint32 NextFrame = 0;     // The next atlas frame to draw.
        uint32 SettledFrames = 0; // Frames in a row with no pipeline build in flight for its materials.
        bool Cleared = false;
    };
    struct AtlasTextures
    {
        ImpostorAtlas Atlas;
        std::array<std::vector<Rendering::TextureViewHandle>, 2> SampleViews;
        uint32 MipCount = 1;
    };
    struct PendingMip
    {
        Rendering::RenderGraph::RGFrameStamp Frame;
        ImpostorAtlasKey Key;
        uint32 Level = 0;
        std::array<const bool*, 2> Recorded{};
    };
    // Draw commands and push constants live until the frame that declared them executes.
    static constexpr uint32 kFrameSlots = 4;
    struct FrameSlot
    {
        std::vector<DrawCommand> Commands;
        std::vector<std::array<std::byte, 128>> PushConstants;
    };

    bool EnsureBakeView(RenderServices& services);
    AtlasTextures& EnsureTextures(Rendering::IDevice& device, const ImpostorAtlasKey& key);
    bool EnsureMipPipeline(Rendering::IDevice& device);
    bool DeclareMip(Rendering::RenderGraph::RGFrame& frame, RenderServices& services, AtlasTextures& textures,
                    const ImpostorAtlasKey& key,
                    const std::array<Rendering::RenderGraph::RGTexture, 2>& resources);
    static void DestroyAtlas(Rendering::IDevice& device, AtlasTextures& textures);
    void FreeTextures(Rendering::IDevice* device, const ImpostorAtlasKey& key);
    bool PartsSettled(RenderServices& services, BakeJob& job);
    void DeclareAtlasFrame(Rendering::RenderGraph::RGFrame& frame, RenderServices& services, const BakeJob& job,
                           Rendering::RenderGraph::RGTexture color, Rendering::RenderGraph::RGTexture normalDepth,
                           Rendering::RenderGraph::RGTexture depth, uint32 atlasFrame, FrameSlot& slot);

    std::mutex m_Mutex;
    ImpostorAtlasCache m_Cache{kDefaultBudgetBytes};
    FlatMap<ImpostorAtlasKey, AtlasTextures> m_Textures;
    std::deque<BakeJob> m_Jobs;
    std::vector<ImpostorAtlasKey> m_Evicted;
    std::vector<ImpostorAtlasKey> m_Finished; // Baked in a declared frame; ready once it ran.
    std::array<FrameSlot, kFrameSlots> m_FrameSlots;
    Rendering::IDevice* m_Device = nullptr;
    std::optional<PendingMip> m_PendingMip;
    Rendering::GraphicsPipelineId m_MipPipeline;
    Rendering::DescriptorSetLayoutDesc m_MipLayout;
    bool m_MipPipelineRequested = false;
    Rendering::CameraId m_Camera{};
    Rendering::ViewId m_View{};
};

} // namespace GameEngine::Engine::Renderer
