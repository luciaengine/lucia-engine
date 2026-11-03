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
    AssetRef<AssetType::Material> Material;
    AssetRef<AssetType::Texture> NormalMap;
    float32 U0=0, V0=0, U1=1, V1=1;
    float32 PixelsPerUnit=16;
    float32 PivotX=.5f, PivotY=.5f;
    float32 NormalStrength=1;
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};
    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Visible=true, FlipX=false, FlipY=false;
    bool Lit=false;
    bool LinearFiltering=true;
};
}
