#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Sprite2D
{

struct SpriteFrame
{
    GUID TextureGuid;
    std::string Name;
    float32 DurationSeconds = 0.1f;
    float32 U0 = 0.0f;
    float32 V0 = 0.0f;
    float32 U1 = 1.0f;
    float32 V1 = 1.0f;

    /**
     * Per-frame pivot in normalized frame space ((0,0) = bottom-left), used
     * only when HasPivot is set; otherwise the component's pivot applies.
     *
     * Trimmed sheets need this: a packer that crops each frame to its opaque
     * bounds leaves different bottom padding per frame, so a single component
     * pivot cannot keep the feet still across an animation.
     */
    bool HasPivot = false;
    float32 PivotX = 0.5f;
    float32 PivotY = 0.5f;
};

struct SpriteFrameAnimation
{
    std::string Name;
    bool Loop = true;
    std::vector<SpriteFrame> Frames;
};

class SpriteFrames : public ::GameEngine::Asset
{
public:
    static constexpr int32 kSchemaVersion = 1;

    SpriteFrames(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::SpriteFrames, path) {}

    bool Load() override;
    bool LoadFromData(std::span<const uint8> data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    const std::vector<SpriteFrameAnimation>& Animations() const { return m_Animations; }
    const SpriteFrameAnimation* FindAnimation(const std::string& name) const;
    const SpriteFrame* Sample(const std::string& animationName, float32 timeSeconds) const;

    void SetAnimationsForTest(std::vector<SpriteFrameAnimation> animations) { m_Animations = std::move(animations); }

private:
    bool Refuse(std::string_view reason);
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    std::vector<SpriteFrameAnimation> m_Animations;
};

} // namespace Sprite2D
} // namespace GameEngine
