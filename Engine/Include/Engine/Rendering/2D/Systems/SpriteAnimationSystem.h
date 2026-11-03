#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Systems.h"
#include "Types/Types.h"
#include "Engine/Rendering/2D/SpriteRenderFeature.h"
#include "Engine/Rendering/SortingLayers.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{
class RenderServices;

/**
 * @brief Advances every AnimatedSprite2D's playback clock and extracts the
 * resolved frames into SpriteRenderFeature.
 *
 * Advancing and extracting share one pass because they share the asset
 * resolution: splitting them would resolve each .spriteframes twice per frame,
 * and there is no earlier phase to advance in anyway — the extraction runs after
 * TransformHierarchy (Extraction phase) because the world matrix it reads is
 * produced there.
 *
 * The renderer has no ECS access of its own, so this is the only seam between
 * the world and the sprite GPU state.
 */
class SpriteAnimationSystem : public ECS::ISystem
{
public:
    explicit SpriteAnimationSystem(RenderServices* renderServices);

    const char* GetName() const override { return "SpriteAnimation"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    /** Frame pixel extent of a sprite sheet, needed to size a quad from PixelsPerUnit. */
    struct TextureSize
    {
        float32 Width = 0.0f;
        float32 Height = 0.0f;
    };

    /** Read dimensions from the asset manager’s current resident texture. */
    std::optional<TextureSize> ResolveTextureSize(const GUID& textureGuid);

    RenderServices* m_RenderServices = nullptr;
    std::vector<SpriteDraw> m_Draws;
    std::vector<std::pair<uint32, SpriteLight>> m_LightEntries;
    std::vector<SpriteLight> m_PackedLights;
    Rendering::SortingLayers m_SortingLayers;
    uint64 m_SortingLayersRevision = UINT64_MAX;
};

} // namespace GameEngine::Engine::Renderer
