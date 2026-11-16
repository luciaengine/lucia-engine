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
enum class OctahedralLayout : uint8_t;
}
namespace Engine::Renderer
{
struct ImpostorAtlas;
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

/// Registers the impostor card material over a baked atlas of the variant: the engine's impostor
/// surface and card modifier, with the atlas textures bound by handle.
void RegisterImpostorMaterial(Engine::Renderer::RenderServices& renderServices,
                              const GUID& material,
                              const Engine::Renderer::ImpostorAtlas& atlas,
                              const Mathematics::BoundingBox& bounds,
                              uint32 framesPerSide,
                              Mathematics::OctahedralLayout layout);

/// True when the wind materials have a pipeline. Until then the static twins draw.
bool VariantMaterialsReady(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);

GUID SelectBarkMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);
GUID SelectLeafMaterial(Engine::Renderer::RenderServices& renderServices, const TreeVariantMaterials& materials);

/// The largest strength scale a part of the tree applies to the wind; 0 without wind.
float WindScaleBound(const EZTree::TreeOptions& options);

} // namespace EZTreeECS
} // namespace GameEngine
