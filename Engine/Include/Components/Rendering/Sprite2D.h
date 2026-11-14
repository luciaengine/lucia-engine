#pragma once
#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/Types.h"
#include "Types/Color.h"
namespace GameEngine::Components
{
// A static atlas quad. Collision is a separate component; no cube geometry.
struct Sprite2D
{
    AssetRef<AssetType::Texture> Texture;
    // @ge-tooltip Optional sprite material: supplies albedo/normal textures, baseColor and opacity. Arbitrary surface shaders are not evaluated by this sprite pass.
    AssetRef<AssetType::Material> Material;
    // @ge-tooltip Normal atlas matching the albedo layout; overrides the material normalMap.
    AssetRef<AssetType::Texture> NormalMap;
    float32 U0=0, V0=0, U1=1, V1=1;
    // @ge-range 1 512
    float32 PixelsPerUnit=16;
    float32 PivotX=.5f, PivotY=.5f;
    // @ge-range 0 4
    float32 NormalStrength=1;
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};
    // @ge-tooltip Camera visibility bitmask; independent from Render Sorting draw order.
    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Visible=true, FlipX=false, FlipY=false;
    // @ge-tooltip Use scene ambient, directional and point lighting with the normal atlas. No shadow casting.
    bool Lit=false;
    bool LinearFiltering=true;
};
}
