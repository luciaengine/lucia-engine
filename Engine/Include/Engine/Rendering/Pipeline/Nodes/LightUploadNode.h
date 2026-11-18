#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace GameEngine::Rendering
{
struct CameraData;
namespace RenderGraph
{
class RGFrame;
} // namespace RenderGraph
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
struct ExtractedLight;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Uploads the per-frame extracted light list into a pipeline-defined buffer (per-view).
//
// This node is intentionally CPU-driven (maps an Upload buffer) as a bootstrap step for Forward+.
// Later iterations can switch to staging buffers + Copy passes, and per-world/per-view filtering.
//
// JSON shape (schemaVersion 2):
// {
//   "id": "LightUpload",
//   "type": "LightUpload",
//   "enabled": true,
//   "buffer": "LightBuffer" // optional (default "LightBuffer")
// }
class LightUploadNode final : public IRenderPipelineNode
{
  public:
    // The std430 record of one point, spot or area light in the LightBuffer. Mirrors
    // light_packed_fields.glsl: a change here changes that file in the same commit.
    struct GPULightPacked
    {
        uint32_t type = 0;
        uint32_t castsShadows = 0;
        uint32_t areaShape = 0;
        uint32_t castsLight = 1;
        float position[3] = {}; // relative to the view's render origin (ViewRelativePoint)
        float range = 0.0f;
        float directionWS[3] = {};
        float intensity = 0.0f;
        float color[3] = {};
        float areaWidth = 1.0f;
        float areaHeight = 1.0f;
        float areaRadius = 0.5f;
        float decay = 2.0f;
        float spotCosInner = 1.0f;
        float spotCosOuter = 1.0f;
        float fogContribution = 1.0f;
        float fogDensityBoost = 0.0f;
        float fogOriginFade = 0.2f;
        float areaRightWS[3] = {1.0f, 0.0f, 0.0f};
        float fogAnisotropy = 0.25f;
        float areaUpWS[3] = {0.0f, 1.0f, 0.0f};
        float falloffMode = 0.0f;
        // Point-shadow atlas slot (-1 = unshadowed) the receiver indexes into ge_pointShadowSlots.
        // The three reserved ints keep the std430 stride 16-aligned.
        int32_t shadowSlot = -1;
        int32_t shadowSlotReserved[3] = {-1, -1, -1};
    };
    // The LightBuffer header ahead of the first record: a uvec4 whose x is the light count.
    static constexpr size_t kLightBufferHeaderBytes = sizeof(uint32_t) * 4;

    // True for a light the LightBuffer holds: a point, spot or area light. The LightUBO carries the
    // directional and ambient lights.
    static bool IsBufferedLight(const ExtractedLight& light);
    // The record of a buffered light for a view whose camera is `camera`.
    static GPULightPacked PackLight(const ExtractedLight& light, const Rendering::CameraData& camera,
                                    int32_t shadowSlot);
    // The most lights a LightBuffer holds; the strongest come first in a world light list.
    static constexpr uint32_t kMaxBufferedLights = 1024;
    // A LightBuffer in the upload ring of a frame. Invalid when the ring is full.
    struct LightBufferUpload
    {
        Rendering::BufferHandle Buffer{};
        uint64_t Offset = 0;
        size_t Bytes = 0;
        bool Valid() const { return Bytes != 0; }
    };
    // Writes the LightBuffer of `lights` (a world light list) for a view whose camera is `camera`. The
    // buffer index of a light is its index among the buffered lights of the list; `shadowSlotOfCluster`
    // gives the point-shadow atlas slot by that index.
    static LightBufferUpload UploadLightBuffer(Rendering::RenderGraph::RGFrame& frame,
                                               std::span<const ExtractedLight> lights,
                                               const Rendering::CameraData& camera,
                                               std::span<const int32_t> shadowSlotOfCluster);

    const char* GetTypeName() const override { return "LightUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "LightBuffer";
    std::string m_ForwardBufferRef;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
