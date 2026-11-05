#include "TileMapECS/TileMapRenderFeature.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewExposureScale.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapParallax.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "TileMapECS/TileSetGpuTables.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace GameEngine
{
namespace TileMapECS
{

using namespace ::GameEngine::Rendering;

namespace
{

constexpr uint32 kInstanceBinding = 0;
constexpr uint32 kTileDescBinding = 1;
constexpr uint32 kAtlasBinding = 2;
constexpr uint32 kAnimFramesBinding = 3;

/** Smallest instance capacity worth allocating; keeps sparse chunks off the churn path. */
constexpr uint32 kMinChunkCapacity = 64;

/** Why a chunk was skipped, so a whole layer lost to depth can be reported. */
enum class ChunkClip
{
    Visible,
    OffScreen,
    OutsideDepthRange,
};

/**
 * Conservative clip-space rejection of a chunk's quad: cull only when all four
 * corners fall outside the same frustum side. A corner at or behind the eye
 * (w <= 0) makes the test meaningless, so it keeps the chunk.
 *
 * Depth is a separate verdict rather than one more side. The layer's ZOffset is
 * authored in tilemap space with no relation to the camera's near/far, and a
 * layer pushed past either plane disappears whole — indistinguishable from a
 * broken tileset unless the reason is named.
 * Reverse-Z: the visible NDC range is z/w in [0,1], near at 1 and far at 0.
 */
ChunkClip ClassifyChunk(const float32* mvp, float32 minX, float32 minY, float32 maxX, float32 maxY,
                        float32 z)
{
    const float32 cornersXY[4][2] = {{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}};
    bool allLeft = true, allRight = true, allBelow = true, allAbove = true;
    bool allNearer = true, allFarther = true;
    for (const auto& corner : cornersXY)
    {
        const float32 x = corner[0], y = corner[1];
        const float32 cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        const float32 cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        const float32 cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
        const float32 cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (cw <= 0.0f)
            return ChunkClip::Visible;
        allLeft &= cx < -cw;
        allRight &= cx > cw;
        allBelow &= cy < -cw;
        allAbove &= cy > cw;
        allNearer &= cz > cw;
        allFarther &= cz < 0.0f;
    }
    if (allNearer || allFarther)
        return ChunkClip::OutsideDepthRange;
    if (allLeft || allRight || allBelow || allAbove)
        return ChunkClip::OffScreen;
    return ChunkClip::Visible;
}

void UnpackRGBA(uint32 rgba, float32 opacity, float32* out)
{
    constexpr float32 kInv255 = 1.0f / 255.0f;
    out[0] = static_cast<float32>((rgba >> 24) & 0xFFu) * kInv255;
    out[1] = static_cast<float32>((rgba >> 16) & 0xFFu) * kInv255;
    out[2] = static_cast<float32>((rgba >> 8) & 0xFFu) * kInv255;
    out[3] = static_cast<float32>(rgba & 0xFFu) * kInv255 * opacity;
}

} // namespace

TileMapRenderFeature::TileMapRenderFeature(Engine::Renderer::RenderServices& services)
    : m_ForwardSubscription(services.RegisterForwardEmit(
          [this](Engine::Renderer::ForwardEmitContext& context) { EmitDraws(context); }, {.WritesDepth = false, .RequiresFrame = true}))
{
}

TileMapRenderFeature::~TileMapRenderFeature()
{
    ReleaseGpuState();
}

bool TileMapRenderFeature::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;
    m_Device = device;

    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg("Shaders/tilemap.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("TileMapRenderFeature: failed to load tilemap.shaderpkg: {}", loadErr);
        return false;
    }
    const auto vs = pkg.stageBytes.find("vs");
    const auto fs = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() || vs->second.empty() ||
        fs->second.empty())
    {
        Logger::Log::Warning("TileMapRenderFeature: tilemap.shaderpkg missing vs/fs stage bytes.");
        return false;
    }

    m_Layout.debugName = "TileMap.Set0";
    m_Layout.bindings = {
        {kInstanceBinding, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
        {kTileDescBinding, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
        {kAtlasBinding, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
        {kAnimFramesBinding, DescriptorType::StorageBuffer, 1, kShaderStageVertex},
        {5, DescriptorType::UniformBuffer, 1, kShaderStageVertex},
    };

    PipelineDesc pipeline{};
    pipeline.type = PipelineType::Graphics;
    pipeline.vertexShader = vs->second;
    pipeline.pixelShader = fs->second;
    pipeline.rasterizationSamples = 1;
    pipeline.SetCullingMode(CullModeFlagBits::None);
    pipeline.EnableDepthTest(true, CompareOp::GreaterOrEqual);
    pipeline.depthStencilState.depthWriteEnable = false;
    pipeline.EnableBlending(true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
    pipeline.pushConstantSize = sizeof(TileMapPushConstants);
    pipeline.pushConstantStagesMask = kShaderStageVertex;
    pipeline.AddDynamicState(DynamicState::Viewport);
    pipeline.AddDynamicState(DynamicState::Scissor);
    pipeline.descriptorSetLayouts.push_back(m_Layout);
    pipeline.debugName = "TileMap";

    m_PipelineId = PipelineDescTranslator::InternGraphics(*device, pipeline);
    if (!m_PipelineId.IsValid())
    {
        Logger::Log::Warning("TileMapRenderFeature: failed to intern the tilemap pipeline.");
        return false;
    }

    // Pixel art is the default authoring case; a linear filter bleeds neighbouring
    // atlas tiles across the tile seam.
    m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("TileMap_AtlasSampler"));
    if (!m_Sampler.IsValid())
        return false;

    m_ShaderMeta = std::move(pkg.meta);
    if (!EnsureQuadIndices())
        return false;
    m_Initialized = true;
    return true;
}

bool TileMapRenderFeature::EnsureQuadIndices()
{
    if (m_QuadIndices.IsValid())
        return true;
    const uint16 indices[] = {0, 1, 2, 3, 4, 5};
    BufferDesc indexDesc{};
    indexDesc.size = sizeof(indices);
    indexDesc.usage = static_cast<uint32>(BufferUsage::Index);
    indexDesc.memoryUsage = BufferMemoryUsage::Upload;
    indexDesc.debugName = "TileMap.QuadIndices";
    m_QuadIndices = m_Device->CreateBuffer(indexDesc);
    if (!m_QuadIndices.IsValid())
        return false;
    m_Device->UpdateBuffer(m_QuadIndices, 0, sizeof(indices), indices);
    return true;
}

void TileMapRenderFeature::ReleaseGpuState()
{
    DestroyChunk(m_PreviewBuffer);
    m_PreviewUploadPending = m_PreviewActive;
    if (!m_Device)
        return;
    if (m_QuadIndices.IsValid())
        m_Device->DestroyBuffer(m_QuadIndices);
    m_QuadIndices = {};
    for (auto& [mapGuid, mapBuffers] : m_MapBuffers)
        for (auto& [layerId, layer] : mapBuffers.Layers)
            for (auto& [packed, chunk] : layer.Chunks)
                DestroyChunk(chunk);
    m_MapBuffers.clear();

    for (auto& [guid, tileSet] : m_TileSets)
    {
        if (tileSet.Descs.IsValid())
            m_Device->DestroyBuffer(tileSet.Descs);
        if (tileSet.Frames.IsValid())
            m_Device->DestroyBuffer(tileSet.Frames);
    }
    m_TileSets.clear();
}

void TileMapRenderFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!device || !m_Initialized)
        return;
    m_Device = device;
    // The rebuild already freed every buffer these handles name, so forget them
    // WITHOUT destroying — a rebuilt device would otherwise be handed handles
    // that still read IsValid() but resolve to nothing.
    m_MapBuffers.clear();
    m_TileSets.clear();
    m_QuadIndices = {};
    EnsureQuadIndices();
    m_TileSetWarned.clear();
    // Every chunk instance buffer is gone, so the store must re-offer them.
    if (auto* store = TileMapRuntimeStore::TryGet())
        for (const TileMapLayerDraw& draw : m_LayerDraws)
            store->MarkAllChunksDirty(draw.Map);
    m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("TileMap_AtlasSampler"));
}

void TileMapRenderFeature::SetPreview(TileMapPreview preview)
{
    m_Preview = std::move(preview);
    m_PreviewActive = !m_Preview.Instances.empty();
    m_PreviewUploadPending = m_PreviewActive;
}

void TileMapRenderFeature::ClearPreview()
{
    m_PreviewActive = false;
    m_PreviewUploadPending = false;
    m_Preview.Instances.clear();
}

void TileMapRenderFeature::SetLayerDraws(uint64 worldId, std::vector<TileMapLayerDraw> draws)
{
    m_WorldId = worldId;
    m_LayerDraws = std::move(draws);
}

const TileMapRenderFeature::TileSetGpu* TileMapRenderFeature::EnsureTileSet(
    Engine::Renderer::RenderServices& rs, const GUID& tileSetGuid)
{
    if (tileSetGuid.IsNull())
        return nullptr;

    auto* store = TileMapRuntimeStore::TryGet();
    const uint64 sourceVersion = store ? store->TileSetVersion(tileSetGuid) : 0;

    if (auto it = m_TileSets.find(tileSetGuid); it != m_TileSets.end())
    {
        if (it->second.SourceVersion != sourceVersion)
        {
            // A re-slice keeps the tile ids and moves the rects behind them, so
            // stale tables draw the wrong part of the atlas rather than nothing.
            // The device retires the buffers past frames-in-flight.
            if (it->second.Descs.IsValid())
                m_Device->DestroyBuffer(it->second.Descs);
            if (it->second.Frames.IsValid())
                m_Device->DestroyBuffer(it->second.Frames);
            m_TileSets.erase(it);
        }
        else
        {
            // The texture service retires and re-mints the GPU texture behind an
            // atlas GUID — a cook completing and adopting its KTX2 artifact, a
            // hot-reload, an eviction. A handle latched at first use would keep
            // sampling whatever texture inherited that slot, which draws the tiles
            // of some other image. Re-resolving is a hash hit on the GUID cache.
            const TextureHandle atlas = rs.Textures().GetOrUpload(it->second.AtlasTexture);
            if (atlas.IsValid())
                it->second.Atlas = atlas;
            return &it->second;
        }
    }

    // Failures are retried, not cached: registry resolution is transient (the
    // asset scan may simply not have reached the file yet), and latching would
    // leave the layer permanently untextured. Only the warning is deduplicated.
    auto& engine = EngineCore::GetInstance();
    AssetMetadata meta{};
    if (!engine.IsInitialized() ||
        !engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(tileSetGuid, meta) ||
        meta.Path.empty())
    {
        return nullptr;
    }

    TileMap::TileSetAsset asset(tileSetGuid, meta.Path);
    if (!asset.Load() || asset.Tiles().empty())
    {
        if (m_TileSetWarned.insert(tileSetGuid).second)
            Logger::Log::Warning("TileMapRenderFeature: tileset {} failed to load or has no tiles",
                                 tileSetGuid.ToString());
        return nullptr;
    }

    // Tile ids are 1-based and dense after a grid slice; the descriptor array
    // is indexed by (id - 1), which is exactly what the vertex shader does.
    // The tables also carry the animation frame walk (see TileSetGpuTables).
    TileSetGpuTables tables;
    if (!BuildTileSetGpuTables(asset, tables))
        return nullptr;

    const auto uploadStorage = [this](const std::vector<float32>& data,
                                      const char* debugName) -> BufferHandle
    {
        BufferDesc bd{};
        bd.size = data.size() * sizeof(float32);
        bd.usage = static_cast<uint32>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = debugName;
        const BufferHandle buffer = m_Device->CreateBuffer(bd);
        if (buffer.IsValid())
            m_Device->UpdateBuffer(buffer, 0, bd.size, data.data());
        return buffer;
    };

    const BufferHandle descBuffer = uploadStorage(tables.Descs, "TileMap_TileDescs");
    if (!descBuffer.IsValid())
        return nullptr;
    const BufferHandle frameBuffer = uploadStorage(tables.Frames, "TileMap_AnimFrames");
    if (!frameBuffer.IsValid())
    {
        m_Device->DestroyBuffer(descBuffer);
        return nullptr;
    }

    TileSetGpu gpu{};
    gpu.Descs = descBuffer;
    gpu.Frames = frameBuffer;
    gpu.SourceVersion = sourceVersion;
    gpu.TileCount = tables.MaxTileId;
    gpu.TilePixelWidth = static_cast<uint32>(std::max(0, asset.TileWidthPx()));
    gpu.TilePixelHeight = static_cast<uint32>(std::max(0, asset.TileHeightPx()));
    gpu.AtlasTexture = asset.AtlasTexture();
    gpu.Atlas = rs.Textures().GetOrUpload(gpu.AtlasTexture);
    if (!gpu.Atlas.IsValid())
    {
        if (m_TileSetWarned.insert(tileSetGuid).second)
            Logger::Log::Warning("TileMapRenderFeature: tileset {} atlas texture is unavailable",
                                 tileSetGuid.ToString());
        m_Device->DestroyBuffer(descBuffer);
        m_Device->DestroyBuffer(frameBuffer);
        return nullptr;
    }

    m_TileSetWarned.erase(tileSetGuid);
    return &m_TileSets.emplace(tileSetGuid, gpu).first->second;
}

void TileMapRenderFeature::DestroyChunk(ChunkBuffer& chunk)
{
    for (ChunkSlot& slot : chunk.Slots)
    {
        if (slot.Buffer.IsValid())
            m_Device->DestroyBuffer(slot.Buffer);
        slot = ChunkSlot{};
    }
    chunk.Count = 0;
    chunk.Cells.clear();
}

void TileMapRenderFeature::UploadChunk(ChunkBuffer& chunk,
                                       const std::vector<TileInstance>& instances)
{
    const uint32 needed = static_cast<uint32>(instances.size());
    chunk.Count = needed;
    chunk.Cells.clear();
    chunk.Cells.reserve(needed);
    for (const auto& instance : instances)
        chunk.Cells.push_back(instance.LocalIndex);
    if (needed == 0)
        return;

    // Write into this frame's slot. The device has already waited on that
    // frame's fence, so nothing in flight is reading it; the other slots still
    // hold what the frames in flight are drawing.
    const uint32 framesInFlight =
        std::clamp(m_Device->GetFramesInFlight(), 1u, IDevice::kMaxSupportedFramesInFlight);
    const uint32 slotIndex = std::min(m_Device->GetFrameIndex(), framesInFlight - 1u);
    ChunkSlot& slot = chunk.Slots[slotIndex];

    if (!slot.Buffer.IsValid() || slot.Capacity < needed)
    {
        // Deferred destroy: the device retires the old buffer past frames-in-flight,
        // so an in-flight frame still reading it is unaffected.
        if (slot.Buffer.IsValid())
            m_Device->DestroyBuffer(slot.Buffer);
        uint32 capacity = kMinChunkCapacity;
        while (capacity < needed)
            capacity *= 2;
        BufferDesc bd{};
        bd.size = static_cast<uint64>(capacity) * sizeof(TileInstance);
        bd.usage = static_cast<uint32>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "TileMap_ChunkInstances";
        slot.Buffer = m_Device->CreateBuffer(bd);
        slot.Capacity = slot.Buffer.IsValid() ? capacity : 0;
    }
    if (!slot.Buffer.IsValid())
    {
        chunk.Count = 0;
        return;
    }
    m_Device->UpdateBuffer(slot.Buffer, 0, needed * sizeof(TileInstance), instances.data());
    chunk.ReadSlot = slotIndex;
    ++m_ChunkUploadCount;
}

void TileMapRenderFeature::FlushDirtyChunks(const GUID& mapGuid, uint32 layerId)
{
    auto* store = TileMapRuntimeStore::TryGet();
    if (!store || !store->ConsumeDirtyChunks(mapGuid, layerId, m_DirtyScratch))
        return;

    const TileMap::TileMapData* data = store->GetMap(mapGuid);
    const TileMap::TileMapLayerData* layer = data ? data->FindLayer(layerId) : nullptr;
    if (!layer)
        return;

    LayerBuffers& buffers = m_MapBuffers[mapGuid].Layers[layerId];
    for (TileMap::ChunkCoord coord : m_DirtyScratch)
    {
        const uint64 packed = TileMap::PackChunkCoord(coord);
        const TileMap::TileMapChunk* chunk = layer->FindChunk(coord);
        if (!chunk)
        {
            // The chunk was compacted away; drop its slot so it stops drawing.
            if (auto it = buffers.Chunks.find(packed); it != buffers.Chunks.end())
            {
                DestroyChunk(it->second);
                buffers.Chunks.erase(it);
            }
            continue;
        }
        BuildChunkInstances(*chunk, m_InstanceScratch);
        UploadChunk(buffers.Chunks[packed], m_InstanceScratch);
    }

    static const bool kUploadTrace = std::getenv("GE_TILEMAP_DEBUG") != nullptr;
    if (kUploadTrace)
    {
        Logger::Log::Info("TileMap.Upload map={} layer={} chunks={} totalUploads={}",
                          mapGuid.ToString(), layerId, m_DirtyScratch.size(), m_ChunkUploadCount);
    }
}

void TileMapRenderFeature::BuildViewDraws(Engine::Renderer::RenderServices& rs,
                                          const float32* viewProj, const float32* cameraPosWS, const float32* cameraForward,
                                          float32 invExposure,
                                          std::vector<TileMapChunkDraw>& outDraws, uint64 viewWorldId)
{
    outDraws.clear();
    if (!HasLayerDraws(viewWorldId))
        return;
    auto* store = TileMapRuntimeStore::TryGet();
    if (!store || !m_Initialized || !viewProj || !cameraPosWS || !cameraForward)
        return;

    // One clock for every draw of the frame: tile animations are globally
    // phase-locked (all instances of a tile show the same frame), and the
    // process-wide clock survives device rebuilds without a phase snap.
    const float32 animationTime = rs.GetShaderAnimationTimeSeconds();

    for (const TileMapLayerDraw& draw : m_LayerDraws)
    {
        const TileMap::TileMapData* data = store->GetMap(draw.Map);
        if (!data)
            continue;
        const TileMap::TileMapLayerData* layer = data->FindLayer(draw.LayerId);
        if (!layer || layer->Type != TileMap::TileMapLayerType::Tiles || !layer->HasAnyCells())
            continue;

        const TileSetGpu* tileSet = EnsureTileSet(rs, layer->TileSetGuid);
        if (!tileSet)
            continue;

        FlushDirtyChunks(draw.Map, draw.LayerId);

        auto mapIt = m_MapBuffers.find(draw.Map);
        if (mapIt == m_MapBuffers.end())
            continue;
        auto layerIt = mapIt->second.Layers.find(draw.LayerId);
        if (layerIt == mapIt->second.Layers.end())
            continue;

        // Parallax (see ComputeParallaxOffset for the convention): a world-space
        // slide added to the layer's translation, resolved per view so every
        // camera gets its own correct offset rather than one extraction-time
        // approximation. Folded in BEFORE the MVP is built, so the chunk culling
        // and the per-chunk push constants below both see the shifted layer —
        // culling a wide background layer against its unshifted position would
        // drop chunks that parallax has pulled back into frame.
        // Quantized to the layer's texel grid so a fractional factor cannot land
        // the background half a texel off the pixel grid.
        const float32 texelW =
            (tileSet->TilePixelWidth > 0)
                ? data->CellWidth / static_cast<float32>(tileSet->TilePixelWidth)
                : 0.0f;
        const float32 texelH =
            (tileSet->TilePixelHeight > 0)
                ? data->CellHeight / static_cast<float32>(tileSet->TilePixelHeight)
                : 0.0f;
        const ParallaxOffset parallax = ComputeParallaxOffset(
            cameraPosWS[0], cameraPosWS[1], draw.ParallaxX, draw.ParallaxY, texelW, texelH);
        auto model = Mathematics::Matrix4x4::FromColumnMajor(draw.Model);
        model[3].x += parallax.X;
        model[3].y += parallax.Y;
        const auto mvp = Mathematics::Matrix4x4::FromColumnMajor(viewProj) * model;

        float32 tint[4];
        UnpackRGBA(draw.TintRGBA, draw.Opacity, tint);
        // Tiles are display-referred art drawn into the scene-linear HDR buffer
        // ahead of exposure + tonemap. Pre-dividing by that exposure keeps a
        // white tile white instead of letting scene luminance decide. Alpha is
        // coverage, so it stays untouched.
        tint[0] *= invExposure;
        tint[1] *= invExposure;
        tint[2] *= invExposure;

        const float32 cellW = data->CellWidth;
        const float32 cellH = data->CellHeight;
        const float32 chunkExtentX = cellW * static_cast<float32>(TileMap::kChunkSize);
        const float32 chunkExtentY = cellH * static_cast<float32>(TileMap::kChunkSize);

        for (const auto& [packed, chunkBuffer] : layerIt->second.Chunks)
        {
            const BufferHandle instances = chunkBuffer.Slots[chunkBuffer.ReadSlot].Buffer;
            if (chunkBuffer.Count == 0 || !instances.IsValid())
                continue;
            const TileMap::ChunkCoord coord = TileMap::UnpackChunkCoord(packed);
            const float32 originX = static_cast<float32>(coord.X) * chunkExtentX;
            const float32 originY = static_cast<float32>(coord.Y) * chunkExtentY;
            const ChunkClip clip = ClassifyChunk(mvp.Data(), originX, originY, originX + chunkExtentX,
                                                 originY + chunkExtentY, draw.ZOffset);
            if (clip == ChunkClip::OutsideDepthRange)
            {
                if (m_DepthClipWarned.emplace(draw.Map, draw.LayerId).second)
                {
                    Logger::Log::Warning(
                        "TileMapRenderFeature: layer {} of tilemap {} sits at Z {}, outside this "
                        "camera's depth range — it will not draw. Move the layer's ZOffset inside "
                        "the camera's near/far.",
                        draw.LayerId, draw.Map.ToString(), draw.ZOffset);
                }
                continue;
            }
            if (clip == ChunkClip::OffScreen)
                continue;

            TileMapChunkDraw chunkDraw{};
            chunkDraw.Entity = draw.Entity;
            std::copy_n(model.Data(), 16, chunkDraw.Push.Model);
            chunkDraw.Push.ChunkOriginCellSize[0] = originX;
            chunkDraw.Push.ChunkOriginCellSize[1] = originY;
            chunkDraw.Push.ChunkOriginCellSize[2] = cellW;
            chunkDraw.Push.ChunkOriginCellSize[3] = cellH;
            chunkDraw.Push.LayerZ[0] = draw.ZOffset;
            chunkDraw.Push.LayerZ[1] = animationTime;
            // Bit pattern, not a conversion: a 29-bit id is exact as a uint and
            // would round as a float. The shader reads it with floatBitsToUint.
            chunkDraw.Push.LayerZ[2] = std::bit_cast<float32>(tileSet->TileCount);
            std::copy_n(tint, 4, chunkDraw.Push.Tint);
            chunkDraw.Instances = instances;
            chunkDraw.TileDescs = tileSet->Descs;
            chunkDraw.AnimFrames = tileSet->Frames;
            chunkDraw.Atlas = tileSet->Atlas;
            chunkDraw.InstanceCount = chunkBuffer.Count;
            // Camera depth orders the layer when ground sorting is off.
            chunkDraw.WorldZ = draw.WorldZ;
            chunkDraw.Sort = Engine::Renderer::MakeRenderSortKey(draw.Sorting,
                (draw.Model[8] * draw.ZOffset + draw.Model[12] - cameraPosWS[0]) * cameraForward[0] +
                (draw.Model[9] * draw.ZOffset + draw.Model[13] - cameraPosWS[1]) * cameraForward[1] +
                (draw.WorldZ - cameraPosWS[2]) * cameraForward[2], cameraPosWS, cameraForward);
            chunkDraw.RenderLayers = draw.RenderLayers;
            AppendChunkDraws(chunkBuffer, chunkDraw, draw.Sorting, outDraws);
        }

        if (!m_PreviewActive || m_Preview.Map != draw.Map || m_Preview.LayerId != draw.LayerId)
            continue;
        if (m_PreviewUploadPending)
        {
            UploadChunk(m_PreviewBuffer, m_Preview.Instances);
            m_PreviewUploadPending = false;
        }
        const BufferHandle previewInstances = m_PreviewBuffer.Slots[m_PreviewBuffer.ReadSlot].Buffer;
        if (m_PreviewBuffer.Count == 0 || !previewInstances.IsValid())
            continue;
        // The ghost is a virtual chunk anchored at the stamp's origin cell, drawn
        // after the layer's own chunks so it blends over them. Same plane: the
        // pass writes no depth, so order alone decides what is on top.
        TileMapChunkDraw ghost{};
        ghost.Entity = draw.Entity;
        std::copy_n(model.Data(), 16, ghost.Push.Model);
        ghost.Push.ChunkOriginCellSize[0] = static_cast<float32>(m_Preview.OriginX) * cellW;
        ghost.Push.ChunkOriginCellSize[1] = static_cast<float32>(m_Preview.OriginY) * cellH;
        ghost.Push.ChunkOriginCellSize[2] = cellW;
        ghost.Push.ChunkOriginCellSize[3] = cellH;
        ghost.Push.LayerZ[0] = draw.ZOffset;
        ghost.Push.LayerZ[1] = animationTime;
        ghost.Push.LayerZ[2] = std::bit_cast<float32>(tileSet->TileCount);
        std::copy_n(tint, 4, ghost.Push.Tint);
        ghost.Push.Tint[3] = m_Preview.Opacity;
        ghost.Instances = previewInstances;
        ghost.TileDescs = tileSet->Descs;
        ghost.AnimFrames = tileSet->Frames;
        ghost.Atlas = tileSet->Atlas;
        ghost.InstanceCount = m_PreviewBuffer.Count;
        ghost.WorldZ = draw.WorldZ;
        ghost.Sort = Engine::Renderer::MakeRenderSortKey(draw.Sorting,
            (draw.Model[8] * draw.ZOffset + draw.Model[12] - cameraPosWS[0]) * cameraForward[0] +
            (draw.Model[9] * draw.ZOffset + draw.Model[13] - cameraPosWS[1]) * cameraForward[1] +
            (draw.WorldZ - cameraPosWS[2]) * cameraForward[2], cameraPosWS, cameraForward);
        ghost.RenderLayers = draw.RenderLayers;
        AppendChunkDraws(m_PreviewBuffer, ghost, draw.Sorting, outDraws);
    }
}

void TileMapRenderFeature::AppendChunkDraws(const ChunkBuffer& chunk, const TileMapChunkDraw& draw,
    const Engine::Renderer::RenderSortingData& sorting, std::vector<TileMapChunkDraw>& outDraws) const
{
    if (!sorting.SortByY)
    {
        outDraws.push_back(draw);
        return;
    }
    const auto& push = draw.Push;
    const auto model = Mathematics::Matrix4x4::FromColumnMajor(push.Model);
    uint32 first = 0;
    while (first < chunk.Count)
    {
        const uint32 cell = chunk.Cells[first];
        const float32 localX = push.ChunkOriginCellSize[0] +
            (static_cast<float32>(cell % TileMap::kChunkSize) + 0.5f) * push.ChunkOriginCellSize[2];
        const float32 localY = push.ChunkOriginCellSize[1] +
            static_cast<float32>(cell / TileMap::kChunkSize) * push.ChunkOriginCellSize[3] + sorting.SortOriginY;
        const float32 groundY = Engine::Renderer::FiniteRenderDepth(
            model.TransformPoint({localX, localY, push.LayerZ[0]}).y + sorting.SectorOffsetY);
        uint32 end = first + 1;
        // An untilted row shares one ground height and one contiguous instance range.
        if (push.Model[1] == 0.0f)
            while (end < chunk.Count && chunk.Cells[end] / TileMap::kChunkSize == cell / TileMap::kChunkSize)
                ++end;
        auto part = draw;
        part.FirstInstance = first;
        part.InstanceCount = end - first;
        if (part.Sort.Grouped)
            part.Sort.MemberDepth = groundY;
        else
            part.Sort.Depth = groundY;
        outDraws.push_back(part);
        first = end;
    }
}

void TileMapRenderFeature::EmitDraws(Engine::Renderer::ForwardEmitContext& context)
{
    using namespace Engine::Renderer;
    if (!context.Services || !context.Frame || context.Purpose != ForwardEmitPurpose::World ||
        !HasLayerDraws() || (!IsInitialized() && !Initialize(context.Device)) || !EnsureQuadIndices())
        return;
    auto& services = *context.Services;
    const auto* view = services.Views().FindViewDesc(context.ViewId);
    if (!view || (view->purpose != ViewPurpose::Game && view->purpose != ViewPurpose::EditorScene))
        return;
    const auto* camera = services.Views().FindCameraData(view->cameraId);
    if (!camera || !HasLayerDraws(view->worldId))
        return;
    const float32 cameraForward[] = {camera->view[2], camera->view[6], camera->view[10]};
    auto& commands = m_ViewCommands[static_cast<uint32>(context.ViewId)];
    BuildViewDraws(services, camera->viewProj, camera->cameraPos, cameraForward,
                   1.0f / ResolveViewExposureScale(services, *view), commands.Draws, view->worldId);
    commands.Buffers.resize(commands.Draws.size());
    commands.Textures.resize(commands.Draws.size());
    for (size_t index = 0; index < commands.Draws.size(); ++index)
    {
        const auto& draw = commands.Draws[index];
        if ((draw.RenderLayers & view->ActiveRenderLayerMask()) == 0u)
            continue;
        auto& buffers = commands.Buffers[index];
        buffers = {{{HashStringId("Instances"), draw.Instances},
                    {HashStringId("TileDescs"), draw.TileDescs},
                    {HashStringId("AnimFrames"), draw.AnimFrames}}};
        auto& texture = commands.Textures[index];
        texture = {HashStringId("uAtlas"), draw.Atlas, m_Sampler};
        SortedTransparentCommand submission{};
        submission.Sorting = draw.Sort;
        submission.Family = HashStringId("TileMap");
        submission.Entity = draw.Entity;
        submission.SubOrder = static_cast<uint32>(index);
        auto& command = submission.Command;
        command.InternedPipeline = m_PipelineId;
        command.PipelineMeta = &m_ShaderMeta;
        command.Geometry.AltGeom.IB = m_QuadIndices;
        command.Geometry.AltGeom.IndexType = IndexType::Uint16;
        command.IndexCount = 6;
        command.InstanceCount = draw.InstanceCount;
        command.FirstInstance = draw.FirstInstance;
        command.Bindings.Buffers = buffers;
        command.Bindings.Textures = std::span<const DrawBindings::TextureEntry>(&texture, 1);
        command.Bindings.PushConstants = std::as_bytes(std::span(&draw.Push, 1));
        services.EmitSortedTransparentCommand(context.ViewId, submission);
        services.EmitForwardSampledRead(*context.Frame, context.ViewId,
            context.Frame->ImportExternalTexture("TileMap.Atlas", draw.Atlas, ResourceState::ShaderResource));
        for (const auto& buffer : buffers)
            services.EmitForwardSampledBufferRead(*context.Frame, context.ViewId,
                context.Frame->ImportExternalBuffer("TileMap.Storage", buffer.Buffer),
                RenderGraph::RGBufferRead::Storage, ForwardBufferReaders::WorldPass);

    }
}

} // namespace TileMapECS
} // namespace GameEngine
