#pragma once
#include "Engine/Rendering/RenderSortKey.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Types/ScopedSubscription.h"
#include <array>

#include "AssetCore/GUID.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TileMap/TileMapChunk.h"
#include "TileMapECS/TileChunkInstances.h"

#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace TileMapECS
{

/**
 * @brief One rendered tilemap layer, extracted from the ECS each frame.
 *
 * `Model` is the owning entity's world matrix (column-major), so the renderer
 * needs no ECS access of its own.
 */
struct TileMapLayerDraw
{
    ECS::EntityHandle Entity{};
    GUID Map;
    float32 Model[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    uint32 LayerId = 0;
    float32 ZOffset = 0.0f;
    /** World Z of the layer plane, used with its world X/Y to resolve view depth. */
    float32 WorldZ = 0.0f;
    Engine::Renderer::RenderSortingData Sorting;
    uint32 RenderLayers = 0xFFFFFFFFu;
    float32 Opacity = 1.0f;
    uint32 TintRGBA = 0xFFFFFFFFu;
    float32 ParallaxX = 1.0f;
    float32 ParallaxY = 1.0f;
};

/**
 * @brief A ghost of cells the editor is about to write: one stamp footprint,
 * anchored at a cell of one layer, drawn through the layer's own tileset at
 * reduced opacity. Instances are addressed like a chunk's, relative to the
 * anchor, so a footprint is limited to kChunkSize on each axis.
 */
/** Opaque enough to read the art, translucent enough to see what it covers. */
inline constexpr float32 kTileMapPreviewOpacity = 0.85f;

struct TileMapPreview
{
    GUID Map;
    uint32 LayerId = 0;
    int32 OriginX = 0;
    int32 OriginY = 0;
    std::vector<TileInstance> Instances;
    float32 Opacity = kTileMapPreviewOpacity;
};

/** Push-constant block; mirrors `TileMapPush` in tilemap.vert. */
struct TileMapPushConstants
{
    float32 Model[16] = {};
    float32 ChunkOriginCellSize[4] = {};
    float32 LayerZ[4] = {};
    float32 Tint[4] = {};
};

static_assert(sizeof(TileMapPushConstants) == 112,
              "push block must match tilemap.vert and stay within the 128-byte guaranteed range");

/** One instanced draw covering the occupied cells of one chunk. */
struct TileMapChunkDraw
{
    ECS::EntityHandle Entity{};
    TileMapPushConstants Push{};
    Rendering::BufferHandle Instances{};
    Rendering::BufferHandle TileDescs{};
    Rendering::BufferHandle AnimFrames{};
    Rendering::TextureHandle Atlas{};
    uint32 InstanceCount = 0;
    /** World Z of the owning layer's plane; Sort holds the resolved view ordering. */
    float32 WorldZ = 0.0f;
    Engine::Renderer::RenderSortKey Sort;
    uint32 RenderLayers = 0xFFFFFFFFu;
};

/**
 * @brief Owns the tilemap GPU state: the unlit instanced-quad pipeline, the
 * per-tileset UV descriptor buffers, and one compacted instance buffer per
 * chunk that survives across frames and is rewritten only when the store
 * reports that chunk dirty.
 */
class TileMapRenderFeature final : public Engine::Renderer::IRenderFeature
{
public:
    explicit TileMapRenderFeature(Engine::Renderer::RenderServices& services);
    ~TileMapRenderFeature() override;

    bool Initialize(Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    /** Extraction output for this frame; replaces the previous frame's list. */
    void SetLayerDraws(uint64 worldId, std::vector<TileMapLayerDraw> draws);
    /**
     * The ghost to draw over its layer until cleared. Uploaded on the next
     * BuildViewDraws that finds the layer; a preview for a layer no view draws
     * costs nothing.
     */
    void SetPreview(TileMapPreview preview);
    void ClearPreview();
    bool HasLayerDraws(uint64 viewWorldId = 0) const
    {
        return !m_LayerDraws.empty() && (viewWorldId == 0 || viewWorldId == m_WorldId);
    }

    /**
     * Syncs GPU state against the store (dirty-chunk re-uploads, tileset
     * descriptors) and emits the chunks visible to `viewProj`. Called at
     * declaration time, once per view.
     */
    /**
     * `invExposure` pre-divides tile color by the exposure the tonemap will
     * apply, so authored 2D art keeps its authored brightness in the HDR chain.
     */
    void BuildViewDraws(Engine::Renderer::RenderServices& rs, const float32* viewProj,
                        const float32* cameraPosWS, const float32* cameraForward, float32 invExposure,
                        std::vector<TileMapChunkDraw>& outDraws, uint64 viewWorldId = 0);


private:
    bool EnsureQuadIndices();
    void EmitDraws(Engine::Renderer::ForwardEmitContext& context);
    struct ViewCommands
    {
        std::vector<TileMapChunkDraw> Draws;
        std::vector<std::array<Engine::Renderer::DrawBindings::BufferEntry, 3>> Buffers;
        std::vector<Engine::Renderer::DrawBindings::TextureEntry> Textures;
    };
    std::unordered_map<uint32, ViewCommands> m_ViewCommands;
    Rendering::ShaderMeta m_ShaderMeta;
    Rendering::BufferHandle m_QuadIndices{};
    ScopedSubscription m_ForwardSubscription;
    struct ChunkSlot
    {
        Rendering::BufferHandle Buffer{};
        uint32 Capacity = 0;
    };

    /**
     * One chunk's instance stream, ringed over the frames in flight.
     *
     * The upload is a mapped memcpy with no fence behind it, and frames
     * N-1..N-(FIF-1) are still sourcing their buffers when it lands. A single
     * buffer would tear mid-stroke — and because the compacted instance list
     * reorders whenever a cell is added or removed, a torn read is a scrambled
     * chunk, not a slightly stale one. `ReadSlot` is the slot the newest data
     * went into; a frame that re-uploads nothing keeps drawing it.
     */
    struct ChunkBuffer
    {
        ChunkSlot Slots[Rendering::IDevice::kMaxSupportedFramesInFlight];
        uint32 ReadSlot = 0;
        uint32 Count = 0;
    };

    struct LayerBuffers
    {
        std::unordered_map<uint64, ChunkBuffer> Chunks; // keyed by PackChunkCoord
    };

    struct MapBuffers
    {
        std::unordered_map<uint32, LayerBuffers> Layers;
    };

    struct TileSetGpu
    {
        Rendering::BufferHandle Descs{};
        Rendering::BufferHandle Frames{}; // animation frame table (never empty; dummy when static)
        Rendering::TextureHandle Atlas{};
        /** Re-resolves Atlas each frame; the service may retire the handle under us. */
        GUID AtlasTexture{};
        /** Store reload counter these tables were built from (TileSetVersion). */
        uint64 SourceVersion = 0;
        /** Highest valid tile id; the shader rejects anything past it. */
        uint32 TileCount = 0;
        /** Atlas texels per tile; with the map's cell size this gives the texel world size. */
        uint32 TilePixelWidth = 0;
        uint32 TilePixelHeight = 0;
    };


    /** Resolves (loading + uploading on first use) the GPU state for a tileset. */
    const TileSetGpu* EnsureTileSet(Engine::Renderer::RenderServices& rs, const GUID& tileSetGuid);

    /** Rewrites the instance buffers of every chunk the store reports dirty. */
    void FlushDirtyChunks(const GUID& mapGuid, uint32 layerId);

    void UploadChunk(ChunkBuffer& chunk, const std::vector<TileInstance>& instances);
    void DestroyChunk(ChunkBuffer& chunk);
    void ReleaseGpuState();

    Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;

    Rendering::GraphicsPipelineId m_PipelineId{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};

    uint64 m_WorldId = 0;
    std::vector<TileMapLayerDraw> m_LayerDraws;
    std::unordered_map<GUID, MapBuffers> m_MapBuffers;
    std::unordered_map<GUID, TileSetGpu> m_TileSets;
    // Tileset load failures already reported. Gates only the log — resolution
    // is retried every frame, so a fixed or late-scanned asset self-heals.
    std::unordered_set<GUID> m_TileSetWarned;
    // (map, layer) pairs already reported as depth-clipped; gates only the log.
    std::set<std::pair<GUID, uint32>> m_DepthClipWarned;

    // Reused across chunks so a steady frame does no per-chunk heap traffic.
    std::vector<TileInstance> m_InstanceScratch;
    std::vector<TileMap::ChunkCoord> m_DirtyScratch;

    // Chunk instance-buffer rewrites since startup, for the upload trace.
    uint64 m_ChunkUploadCount = 0;

    TileMapPreview m_Preview;
    bool m_PreviewActive = false;
    ChunkBuffer m_PreviewBuffer;
    // Set by SetPreview, cleared by the upload: one upload per change, not per view.
    bool m_PreviewUploadPending = false;
};

} // namespace TileMapECS
} // namespace GameEngine
