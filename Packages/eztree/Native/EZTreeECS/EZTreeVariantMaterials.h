#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;
}
namespace EZTree
{
struct TreeOptions;
}
namespace Mathematics
{
struct BoundingBox;
}

namespace EZTreeECS
{

/// The runtime materials of one tree variant. The GUIDs come from the variant slot: the
/// next variant that takes a freed slot registers its materials over the same GUIDs, so
/// the material count stays at the peak number of live variants.
struct TreeVariantMaterials
{
    GUID Bark;
    GUID BarkStatic; // Bark without wind: drawn while the wind material cannot compile, and baked into the impostor.
    GUID Leaves;
    GUID LeavesStatic;
    GUID Trellis;
    GUID Impostor; // The card that draws the variant's baked atlas beyond the switch distance.
};

TreeVariantMaterials VariantMaterialGuids(uint32 slot);

/// Registers the bark, leaf and trellis materials of a variant from its options and the
/// bounds of its generated mesh. The wind in them is the authored wind; ez_tree_wind.glsl
/// resolves the wind volumes at each instance.
void RegisterVariantMaterials(Engine::Renderer::RenderServices& renderServices,
                              const TreeVariantMaterials& materials,
                              const EZTree::TreeOptions& options,
                              const Mathematics::BoundingBox& bounds);

/// True when the wind materials have a pipeline. Until then the static twins draw.
bool VariantMaterialsReady(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);

GUID SelectBarkMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);
GUID SelectLeafMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);

/// The largest strength scale a part of the tree applies to the wind; 0 without wind.
float WindScaleBound(const EZTree::TreeOptions& options);

/// How far the wind volumes can move a vertex of the tree beyond its authored wind, local meters:
/// the culling sphere grows by this. The wind material's @deformationEnvelope covers the authored
/// wind in the cull kernels, so this term holds only the volumes' share. volumeSpeedBound: the
/// strongest wind the world's volumes can add, m/s (each volume at full weight).
float WindVolumeCullMargin(const EZTree::TreeOptions& options, float volumeSpeedBound);

} // namespace EZTreeECS
} // namespace GameEngine
