#pragma once
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Types/ScopedSubscription.h"
#include <array>
#include "Engine/Rendering/RenderSortKey.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Vector3.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{

class RenderServices;

/**
 * @brief One sprite quad resolved by extraction: world placement, the atlas it
 * samples, and the frame's UV rect.
 *
 * Center/AxisX/AxisY are already world space, so the renderer needs no ECS or
 * transform access of its own. AxisX/AxisY are half-extents: the quad spans
 * Center ± AxisX ± AxisY, and both carry the entity's rotation and scale.
 * View-independent — the node draws the same list for every view.
 */
inline constexpr uint32 kMaxSpriteLights = 64;

struct SpriteLight
{
    float32 PositionType[4]{};
    float32 ColorRange[4]{};
    float32 DirectionFalloff[4]{};
};

struct SpriteDraw
{
    RenderSortingData Sorting;
    GUID Texture;
    GUID NormalMap;
    bool Lit = false;
    bool LinearFiltering = false;
    float32 NormalStrength = 1;
    uint32 RenderLayers = 0xFFFFFFFFu;
    float32 Center[3] = {};
    float32 AxisX[3] = {};
    float32 AxisY[3] = {};
    /** (u0, v0, u1, v1) with FlipX/FlipY already applied. */
    float32 UVRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    /** Linear rgba; alpha carries the tint's opacity. */
    float32 Tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    /**
     * Owning entity, used only to break ties in the draw sort.
     *
     * Sorting on world Z alone leaves equal-Z sprites in ECS iteration order,
     * which moves with archetype changes and entity destruction — so two sprites
     * at the same Z swap places between frames and flicker. This is the sprite
     * equivalent of the tile layers' (Map, LayerId) key.
     */
    ECS::EntityHandle Entity{};
};

/** Push-constant block; mirrors `SpritePush` in sprite2d.vert. */
struct SpritePushConstants
{
    float32 InvExposure[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    /** x = the batch's first instance index; the rest is padding. */
    uint32 Batch[4] = {};
};

static_assert(sizeof(SpritePushConstants) == 32,
              "push block must match sprite2d.vert and stay within the 128-byte guaranteed range");

/** One instanced draw over a run of instances sharing an atlas texture. */
struct SpriteBatch
{
    ECS::EntityHandle Entity{};
    RenderSortKey Sort;
    ::GameEngine::Rendering::TextureHandle Atlas{};
    ::GameEngine::Rendering::TextureHandle Normal{};
    ::GameEngine::Rendering::BufferHandle Lights{};
    bool LinearFiltering = false;
    ::GameEngine::Rendering::BufferHandle Instances{};
    uint32 FirstInstance = 0;
    uint32 InstanceCount = 0;
    /**
     * World Z of the batch's first sprite, for the merged 2D sort.
     *
     * A batch is a run of adjacent same-atlas sprites taken from an already
     * Z-sorted list, so it spans a contiguous Z range and one key orders it
     * against tile layers correctly.
     */
    float32 WorldZ = 0.0f;
};

// Shared by production batching and regression tests. A batch may never span
// sort keys: a tile chunk can belong between any two distinct keys.
inline bool CanAppendSpriteBatch(const SpriteBatch& batch,
    ::GameEngine::Rendering::TextureHandle atlas, ::GameEngine::Rendering::TextureHandle normal,
    const RenderSortKey& sort, bool linear)
{
    return batch.Atlas==atlas && batch.Normal==normal && batch.Sort==sort && batch.LinearFiltering==linear;
}

/**
 * @brief Owns the sprite GPU state: the unlit instanced-quad pipeline and a
 * per-view ring of instance storage buffers.
 *
 * The pass carries no depth attachment (same reason as the tilemap pass — the
 * effective color target may be single-sample while scene depth is still
 * multisampled), so ordering is resolved per view before batching.
 * Consecutive draws sharing an atlas collapse into one instanced draw, so a
 * screen full of one character sheet costs one draw call without ever
 * reordering across a Z boundary.
 */
class SpriteRenderFeature final : public IRenderFeature
{
public:
    explicit SpriteRenderFeature(RenderServices& services);
    ~SpriteRenderFeature() override;

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    void OnDeviceRebuilt(::GameEngine::Rendering::IDevice* device) override;

    /** Extraction output for this frame; replaces the previous frame's list. */
    void SetSpriteLights(std::vector<SpriteLight>& lights) { m_Lights.swap(lights); }
    void SetSpriteDraws(uint64 worldId, std::vector<SpriteDraw>& draws);
    bool HasSpriteDraws(uint64 viewWorldId = 0) const
    {
        return !m_SpriteDraws.empty() && (viewWorldId == 0 || viewWorldId == m_WorldId);
    }

    /**
     * Resolves atlas textures, uploads this frame's instances into `viewId`'s
     * ring slot, and emits the batches. Called at declaration time, once per
     * view. Leaves `outBatches` empty when nothing can draw.
     */
    void BuildViewDraws(RenderServices& rs, uint32 viewId, std::vector<SpriteBatch>& outBatches);

private:
    void EmitDraws(ForwardEmitContext& context);
    bool EnsureQuadIndices();
    struct ViewCommands
    {
        std::vector<SpriteBatch> Batches;
        std::vector<SpritePushConstants> Pushes;
        std::vector<std::array<DrawBindings::BufferEntry, 2>> Buffers;
        std::vector<std::array<DrawBindings::TextureEntry, 2>> Textures;
    };
    std::unordered_map<uint32, ViewCommands> m_ViewCommands;
    ::GameEngine::Rendering::ShaderMeta m_ShaderMeta;
    ::GameEngine::Rendering::BufferHandle m_QuadIndices{};
    ScopedSubscription m_ForwardSubscription;
    /** Mirrors `SpriteInstance` in sprite2d.vert (std430, 96 bytes). */
    struct SpriteInstance
    {
        float32 Center[4];
        float32 AxisX[4];
        float32 AxisY[4];
        float32 UVRect[4];
        float32 Tint[4];
        float32 Surface[4];
    };

    /** Ring of per-frame instance buffers for one view. */
    struct ViewRing
    {
        std::vector<::GameEngine::Rendering::BufferHandle> Buffers;
        std::vector<uint32> Capacity;
        std::vector<::GameEngine::Rendering::BufferHandle> Lights;
        std::vector<size_t> LightCapacity;
    };

    /** Buffer for this view's current frame slot, grown to fit. Invalid on failure. */
    ::GameEngine::Rendering::BufferHandle AcquireInstanceBuffer(uint32 viewId, uint32 instanceCount);
    void ReleaseGpuState();
    std::vector<SpriteLight> m_Lights;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;

    ::GameEngine::Rendering::GraphicsPipelineId m_PipelineId{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout{};
    ::GameEngine::Rendering::SamplerHandle m_Sampler{};
    ::GameEngine::Rendering::SamplerHandle m_LinearSampler{};

    // Sized from the device's frames-in-flight so a slot is never rewritten
    // while the GPU may still be reading it.
    uint32 m_FramesInFlight = 1;
    std::unordered_map<uint32, ViewRing> m_ViewRings;

    uint64 m_WorldId = 0;
    std::vector<SpriteDraw> m_SpriteDraws;
    // Reused across frames so a steady frame does no heap traffic.
    std::vector<SpriteInstance> m_InstanceScratch;
    struct ViewDraw
    {
        const SpriteDraw* Draw;
        RenderSortKey Sort;
        Mathematics::Vector3 Center;
    };
    std::vector<ViewDraw> m_ViewDraws;
};

} // namespace GameEngine::Engine::Renderer
