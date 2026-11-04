#include "Engine/Rendering/2D/Systems/SpriteAnimationSystem.h"

#include "Sprite2D/SpriteFrames.h"
#include "Sprite2D/SpritePlayback.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Assets/MaterialAsset.h"
#include "Components/Rendering/Sprite2D.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/2D/SpriteQuad.h"
#include "Engine/Rendering/RenderSorting.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Engine/Rendering/2D/SpriteRenderFeature.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

namespace
{

void CopyTint(const ColorLinear& tint, float32* out)
{
    out[0] = tint.r;
    out[1] = tint.g;
    out[2] = tint.b;
    out[3] = tint.a;
}

template <class T>
std::shared_ptr<T> LoadTyped(AssetManager& assets, const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;
    SharedPtr<Asset> asset = assets.GetAsset(guid);
    if (!asset)
    {
        assets.LoadAsset(guid, AssetLoadResultCallback{}, AssetLoadPriority::High);
        return nullptr;
    }
    return std::dynamic_pointer_cast<T>(asset);
}

} // namespace

SpriteAnimationSystem::SpriteAnimationSystem(RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
    // Systems are constructed on the owning thread before extraction jobs run.
    if (m_RenderServices)
        m_RenderServices->EnsureFeature<SpriteRenderFeature>(*m_RenderServices);
}

std::optional<SpriteAnimationSystem::TextureSize> SpriteAnimationSystem::ResolveTextureSize(
    const GUID& textureGuid)
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return std::nullopt;

    // Failures are not cached: an asset scan may simply not have reached the
    // file yet, and latching would leave the sprite permanently unsized.
    auto texture = LoadTyped<TextureAsset>(engine.GetAssetManager(), textureGuid);
    if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
        return std::nullopt;

    TextureSize size{};
    size.Width = static_cast<float32>(texture->GetWidth());
    size.Height = static_cast<float32>(texture->GetHeight());
    return size;
}

void SpriteAnimationSystem::Update(ECS::World& world, float32 deltaTime)
{
    if (!m_RenderServices)
        return;

    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;
    auto& assets = engine.GetAssetManager();

    // Sorted below, so the push order here is irrelevant.
    auto& draws = m_Draws;
    draws.clear();
    const uint64 revision = Rendering::SortingLayers::Revision();
    if (revision != m_SortingLayersRevision)
    {
        m_SortingLayers = Rendering::SortingLayers::Current();
        m_SortingLayersRevision = revision;
    }
    const auto& layers = m_SortingLayers;

    world
        .Query<ECS::Write<Components::AnimatedSprite2D>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>()
        .Each(
            [&](ECS::EntityHandle entity, ECS::Writable<Components::AnimatedSprite2D> spriteRow,
                const Components::WorldTransform& worldXf)
            {
                const auto& sprite = spriteRow.Get();
                if (sprite.SpriteFrames.IsNull() || sprite.AnimationName[0] == '\0')
                    return;

                auto frames =
                    LoadTyped<Sprite2D::SpriteFrames>(assets, sprite.SpriteFrames.ToGuid());
                if (!frames)
                    return;
                const std::string animationName(sprite.GetAnimationName());
                const Sprite2D::SpriteFrameAnimation* animation =
                    frames->FindAnimation(animationName);
                if (!animation || animation->Frames.empty())
                    return;

                // Looping is the animation's property, not the entity's: a
                // one-shot clamps and holds its final frame.
                if (sprite.Playing)
                {
                    const float32 nextTime = Sprite2D::AdvanceSpriteTime(
                        *animation, sprite.TimeSeconds, deltaTime, sprite.SpeedScale);
                    if (nextTime != sprite.TimeSeconds)
                        spriteRow.Mutable().TimeSeconds = nextTime;
                }

                const std::size_t frameIndex =
                    Sprite2D::SpriteFrameIndexAt(*animation, sprite.TimeSeconds);
                if (frameIndex == Sprite2D::kInvalidSpriteFrameIndex)
                    return;
                const Sprite2D::SpriteFrame& frame = animation->Frames[frameIndex];

                // Everything below is render-only: a hidden or degenerate sprite
                // still advanced its clock above, so unhiding it does not snap.
                if (!sprite.Visible || frame.TextureGuid.IsNull() || sprite.PixelsPerUnit <= 0.0f)
                    return;

                const auto sheet = ResolveTextureSize(frame.TextureGuid);
                if (!sheet)
                    return;

                const float32 framePixelW = std::abs(frame.U1 - frame.U0) * sheet->Width;
                const float32 framePixelH = std::abs(frame.V1 - frame.V0) * sheet->Height;
                if (framePixelW <= 0.0f || framePixelH <= 0.0f)
                    return;

                // A trimmed frame carries its own pivot; otherwise the whole
                // sprite shares the component's.
                const float32 pivotX = frame.HasPivot ? frame.PivotX : sprite.PivotX;
                const float32 pivotY = frame.HasPivot ? frame.PivotY : sprite.PivotY;

                const SpriteQuadPlacement placement =
                    BuildSpriteQuad(worldXf.matrix, framePixelW / sprite.PixelsPerUnit,
                                    framePixelH / sprite.PixelsPerUnit, pivotX, pivotY,
                                    sprite.FlipX, sprite.FlipY);

                SpriteDraw draw{};
                draw.Texture = frame.TextureGuid;
                draw.RenderLayers = sprite.RenderLayers;
                draw.Entity = entity;
                draw.Sorting = ResolveRenderSorting(world, entity, layers);
                for (int32 i = 0; i < 3; ++i)
                {
                    draw.Center[i] = placement.Center[i];
                    draw.AxisX[i] = placement.AxisX[i];
                    draw.AxisY[i] = placement.AxisY[i];
                }

                // Flips swap the rect's edges, so the shader's corner->UV lerp
                // needs no flip branch of its own.
                const float32 u0 = sprite.FlipX ? frame.U1 : frame.U0;
                const float32 u1 = sprite.FlipX ? frame.U0 : frame.U1;
                const float32 v0 = sprite.FlipY ? frame.V1 : frame.V0;
                const float32 v1 = sprite.FlipY ? frame.V0 : frame.V1;
                draw.UVRect[0] = u0;
                draw.UVRect[1] = v0;
                draw.UVRect[2] = u1;
                draw.UVRect[3] = v1;

                CopyTint(sprite.Tint, draw.Tint);
                draws.push_back(draw);
            });

    world.Query<ECS::Read<Components::Sprite2D>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>().Each([&](ECS::EntityHandle entity, const Components::Sprite2D& sprite,
                                         const Components::WorldTransform& xf) {
        if (!sprite.Visible || !std::isfinite(sprite.PixelsPerUnit) || sprite.PixelsPerUnit<=0) return;
        SpriteDraw draw{};
        draw.Texture=sprite.Texture.ToGuid(); draw.NormalMap=sprite.Lit ? sprite.NormalMap.ToGuid() : GUID::Null();
        draw.LinearFiltering=sprite.LinearFiltering;
        draw.Lit=sprite.Lit; draw.NormalStrength=std::isfinite(sprite.NormalStrength) ? std::max(0.f,sprite.NormalStrength) : 1.0f;
        draw.RenderLayers=sprite.RenderLayers;
        float u0=sprite.U0,v0=sprite.V0,u1=sprite.U1,v1=sprite.V1;
        CopyTint(sprite.Tint,draw.Tint);
        const auto material = LoadTyped<MaterialAsset>(assets, sprite.Material.ToGuid());
        if (!sprite.Material.IsNull() && !material) return;
        if (material) {
            const auto& doc=material->GetDocument();
            auto texture=[&](const char* name) {auto it=doc.textures.find(name);return it==doc.textures.end()?GUID::Null():GUID(it->second);};
            if(draw.Texture.IsNull()) draw.Texture=texture("albedoMap");
            if(draw.Lit && draw.NormalMap.IsNull()) draw.NormalMap=texture("normalMap");
            if (auto it=doc.properties.find("baseColor");it!=doc.properties.end())
                if(auto* c=std::get_if<std::vector<float>>(&it->second);c && c->size()>=3)
                    for(int j=0;j<3;++j)draw.Tint[j]*=(*c)[j];
            draw.Tint[3]*=doc.DefaultOpacity();
            if(auto it=doc.textureTransforms.find("albedoMap");it!=doc.textureTransforms.end()) {
                const auto& t=it->second;
                u0=t[2]+t[0]*u0;u1=t[2]+t[0]*u1;v0=t[6]+t[5]*v0;v1=t[6]+t[5]*v1;
            }
        }
        if(draw.Texture.IsNull())return;
        const auto sheet=ResolveTextureSize(draw.Texture);if(!sheet)return;
        const float w=std::abs(u1-u0)*sheet->Width/sprite.PixelsPerUnit;
        const float h=std::abs(v1-v0)*sheet->Height/sprite.PixelsPerUnit;
        if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)return;
        const auto placement=BuildSpriteQuad(xf.matrix,w,h,sprite.PivotX,sprite.PivotY,sprite.FlipX,sprite.FlipY);
        draw.Entity=entity;draw.Sorting=ResolveRenderSorting(world,entity,layers);
        for(int j=0;j<3;++j){draw.Center[j]=placement.Center[j];draw.AxisX[j]=placement.AxisX[j];draw.AxisY[j]=placement.AxisY[j];}
        draw.UVRect[0]=sprite.FlipX?u1:u0;draw.UVRect[2]=sprite.FlipX?u0:u1;
        draw.UVRect[1]=sprite.FlipY?v1:v0;draw.UVRect[3]=sprite.FlipY?v0:v1;
        draws.push_back(draw);
    });

    // Empty scenes retain no draws. Pipeline and GPU resources stay lazy until
    // a view actually draws a visible sprite.
    if (draws.empty())
    {
        if (auto* feature = m_RenderServices->GetFeature<SpriteRenderFeature>())
            feature->SetSpriteDraws(world.GetWorldId(), draws);
        return;
    }


    auto& lights = m_LightEntries;
    lights.clear();
    const bool needsLight=std::any_of(draws.begin(),draws.end(),[](const auto& d){return d.Lit;});
    if(needsLight)world.Query<ECS::Read<Components::Light>,ECS::Read<Components::WorldTransform>>()
      .Without<ECS::Disabled>().Each([&](ECS::EntityHandle entity,const Components::Light& light,const Components::WorldTransform& xf){
        if(!light.CastsLight||light.Intensity<=0)return;
        if(light.Type!=Components::LightType::Directional && light.Type!=Components::LightType::Point && light.Type!=Components::LightType::Ambient)return;
        SpriteLight out{};float color[3],intensity;
        Components::ResolveLightColorIntensity(light,color,intensity);
        for(int i=0;i<3;++i){out.PositionType[i]=xf.matrix[12+i];out.DirectionFalloff[i]=xf.matrix[8+i];out.ColorRange[i]=color[i]*intensity;}
        out.PositionType[3]=float(light.Type);out.ColorRange[3]=std::max(.001f,light.Range);
        out.DirectionFalloff[3]=float(light.Falloff);
        lights.emplace_back(entity.id,out);
      });
    std::sort(lights.begin(),lights.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    auto& packed = m_PackedLights;
    packed.clear();
    for (const auto& light : lights)
    {
        if (packed.size() == kMaxSpriteLights) break;
        packed.push_back(light.second);
    }
    auto* feature = m_RenderServices->GetFeature<SpriteRenderFeature>();
    if (!feature) return;
    feature->SetSpriteLights(packed);
    feature->SetSpriteDraws(world.GetWorldId(), draws);
}

} // namespace GameEngine::Engine::Renderer
