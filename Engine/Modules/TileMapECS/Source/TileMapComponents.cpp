// Manual reflection + cross-DLL template instantiation for TileMapLayerRef.
//
// The build-time ComponentScanner skips this struct: its AssetRef<> field is a
// templated type the scanner's lightweight C++ parser can't resolve, so it omits
// the struct rather than emit an incomplete field list. Components with asset
// references are registered by hand (same approach as LensFlareSource and the
// terrain modifiers).

#include "Components/ComponentRegistration.h" // GE_REGISTER_COMPONENT
#include "Components/TileMap/TileMapCollider.h"
#include "Components/TileMap/TileMapLayerRef.h"

#include "ECS/ECSTemplates.h" // GE_INSTANTIATE_ENGINE_COMPONENT

// Field table + factory registration (Add Component menu + scene serialization).
GE_REGISTER_COMPONENT(GameEngine::Components::TileMapLayerRef, TileMap, LayerId, Visible, Opacity,
                      TintRGBA, ParallaxX, ParallaxY, ZOffset, RenderLayers);

// Inspector polish: slider bounds + tooltips (must follow registration).
namespace
{
using GameEngine::Components::SetReflectedFieldRange;
using GameEngine::Components::SetReflectedFieldTooltip;
using GameEngine::Components::TileMapLayerRef;

const bool kTileMapLayerRefPolish =
    SetReflectedFieldRange<TileMapLayerRef>("Opacity", 0.0f, 1.0f) &&
    SetReflectedFieldRange<TileMapLayerRef>("ParallaxX", 0.0f, 2.0f) &&
    SetReflectedFieldRange<TileMapLayerRef>("ParallaxY", 0.0f, 2.0f) &&
    SetReflectedFieldTooltip<TileMapLayerRef>(
        "LayerId", "Stable id of the layer to draw within the tilemap asset.") &&
    SetReflectedFieldTooltip<TileMapLayerRef>(
        "TintRGBA", "0xRRGGBBAA multiplied into every tile's texel color.") &&
    SetReflectedFieldTooltip<TileMapLayerRef>(
        "ParallaxX", "Fraction of camera motion this layer follows: 1 is world-locked, "
                     "0 is pinned to the camera.") &&
    SetReflectedFieldTooltip<TileMapLayerRef>(
        "ZOffset", "Local depth offset, so several layers on one entity sort against each other.");
} // namespace

// Explicit instantiation of the type-erased handler + World accessors so the
// component works across the Engine DLL boundary.
namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::TileMapLayerRef);
// TileMapCollider itself is scanner-registered (plain POD header); only the
// cross-DLL accessor instantiation lives here.
GE_INSTANTIATE_ENGINE_COMPONENT(Components::TileMapCollider);
} // namespace GameEngine::ECS
