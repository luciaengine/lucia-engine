#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Octahedral.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include <atomic>
#include <span>
#include <string_view>
#include <vector>

namespace GameEngine::Engine::Renderer
{

class Material;
class RenderServices;
struct ImpostorAtlas;

struct ImpostorFoliageSway
{
    float BaseY = 0.0f;
    float Height = 1.0f;
    float MaxDisplacement = 0.0f;

    bool operator==(const ImpostorFoliageSway&) const = default;
};

/// The impostor last stage on the GPU (docs/development/tree-impostor-stage.mdx, Step 4). A mesh
/// part linked to a card draws that card past the switch coverage: the scatter
/// (draw_command_scatter.comp) selects per instance and moves the primary part's record to the
/// card's batch row. This feature owns the card quad, the switch constants and the links.
class ImpostorStage final : public IRenderFeature
{
public:
    /// Whether this renderer can draw and request impostors.
    static bool IsEnabled(const RenderServices& services);

    /// Automatic handoff coverage shared by view selection and atlas demand.
    static float DefaultSwitchCoverage(const RenderServices& services, Rendering::ViewId viewId);

    /// Extra coverage the card can retain, within the coarsest mesh LOD.
    static constexpr float kSwitchHysteresis = 0.15f;

    /// Registers the card quad, a [0,1] UV square that impostor_card.glsl places, and publishes its
    /// GPU-scene index to the slices. Call each frame before linking; a dedup hit costs a lookup.
    void EnsureCardMesh(RenderServices& services);
    Rendering::MeshGPUHandle CardMesh() const { return m_CardMesh; }

    /// Sets the impostor fields of a slice's LOD parameters. GE_IMPOSTORS=0 leaves every linked
    /// mesh on its mesh, as a reference for comparisons.
    void ApplyToSlice(Rendering::GPUDrawStreamBuilder::ViewLODParams& lod, const RenderServices& services,
                      Rendering::ViewId viewId) const;

    /// The view's submitted cards may replace their source meshes in the
    /// corresponding conventional shadow slices only when these heads are warm.
    bool AreShadowCardsReady(RenderServices& services, Rendering::ViewId viewId, bool directional) const;

    enum class CardMaterialStyle { Model, PackedColor, ShadowCoverage };

    /// Registers (or points at a new atlas) the card material `material` of an atlas: the engine's
    /// Surfaces/impostor.glsl and VertexModifiers/impostor_card.glsl, fitted to the sphere
    /// (`center`, `radius`) in object space. Returns the material once its pipeline is built.
    static const Material* RegisterCardMaterial(RenderServices& services, const GUID& material, std::string_view name,
                                                const ImpostorAtlas& atlas, const Mathematics::Vector3& center,
                                                float radius, uint32 framesPerSide, Mathematics::OctahedralLayout layout,
                                                const ImpostorFoliageSway& sway = {},
                                          CardMaterialStyle style = CardMaterialStyle::Model);

    /// The GPU-scene mesh indices of `parts`, in order; empty when a part is not resident yet.
    static std::vector<uint32> PartMeshIndices(const RenderServices& services,
                                               std::span<const Rendering::MeshGPUHandle> parts);

    /// Links the parts of one object (their GPU-scene mesh indices, PartMeshIndices) to `card`.
    /// parts[0] is the primary part, which draws the card; the others draw nothing past the switch.
    /// Every part must have the same instance sphere, so all parts switch in the same frame. Links
    /// all parts or none: returns false when the card cannot draw.
    bool Link(RenderServices& services, std::span<const uint32> partMeshes, const Material& card,
              float switchCoverage = 0.0f, bool requiresPlacement = false) const;

    /// Removes the links of the parts; they draw their meshes at every distance.
    static void Unlink(RenderServices& services, std::span<const uint32> partMeshes);

    /// Names each card to the views of `worldId`, so the world passes draw the card rows that the
    /// scatter fills. Submit only to views with drawable card color and camera-prepass pipelines.
    /// Call once per frame with every card the caller has linked.
    void SubmitCards(RenderServices& services, uint64 worldId, std::span<const Material* const> cards) const;

    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    static constexpr uint32 kNoCardMesh = 0xFFFFFFFFu;
    // Read by the slice registration, written by the system that ensures the quad.
    std::atomic<uint32> m_CardMeshIndex{kNoCardMesh};
    Rendering::MeshGPUHandle m_CardMesh;
};

} // namespace GameEngine::Engine::Renderer
