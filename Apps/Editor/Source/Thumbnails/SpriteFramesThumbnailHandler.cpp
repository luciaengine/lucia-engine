#include "Thumbnails/SpriteFramesThumbnailHandler.h"

#include "Thumbnails/ThumbnailImageCache.h"
#include "Thumbnails/ThumbnailImageOps.h"

#include "Sprite2D/SpriteFrames.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "JobSystem/WorkStealingThreadPool.h"
#include <unordered_set>
#include <utility>

namespace GameEngine
{
namespace
{

using Editor::DecodeImage;
using Editor::HashCombine;
using Editor::HashPathAndWriteTime;
using Editor::MirrorHorizontal;
using Editor::RemoveStaleCacheFiles;
using Editor::ResolveAssetPathByGuid;
using Editor::ThumbnailImage;
using Editor::WritePng;

// Matches the tile thumbnails: shown at up to 128 logical px in the asset grid,
// so 256 keeps a high-DPI cell crisp without making the cache heavy.
constexpr int32 kMaxEdgePx = 256;

// The largest box that shows a preview is the 64px hierarchy icon; scaling a
// trimmed frame up to at least this keeps that case free of sampler blur.
constexpr int32 kMinEdgePx = 128;

// Alpha at or below this is packing dust — antialiased fringes that would drag
// the trimmed bounds back out to the untrimmed frame.
constexpr uint8 kOpaqueAlphaThreshold = 8u;

// Breathing room around the trimmed art, as a fraction of its long edge, so a
// row preview does not touch the label beside it. ~2px at a 32px icon.
constexpr float32 kMarginFraction = 0.06f;

// Folded into the cache stamp so a change to the steps above regenerates
// previews that were written by an older pipeline. Their file names carry
// source write times, which a code change does not move.
constexpr int32 kPreviewFormatVersion = 2;

constexpr const char* kCachePrefix = "spritethumb_";

/**
 * Names the crop a cache file belongs to:
 * "spritethumb_<assetHash>_<animationAndFacingHash>_".
 *
 * The variant is part of the identity, not just of the content stamp: one asset
 * legitimately has a live cache file per animation and facing, and a sweep keyed
 * on the asset alone would delete the crops its sibling rows are showing.
 */
std::string CacheIdentityPrefix(const std::filesystem::path& assetPath,
                                const std::string& animationName, bool flipX)
{
    std::size_t variant = std::hash<std::string>{}(animationName);
    HashCombine(variant, static_cast<std::size_t>(flipX ? 1u : 0u));
    return std::string(kCachePrefix) +
           std::to_string(std::hash<std::string>{}(assetPath.generic_string())) + "_" +
           std::to_string(variant) + "_";
}

/** The animation the preview represents: the named one, else the first defined. */
const Sprite2D::SpriteFrameAnimation* PickAnimation(const Sprite2D::SpriteFrames& frames,
                                                     const std::string& animationName)
{
    if (!animationName.empty())
    {
        if (const Sprite2D::SpriteFrameAnimation* named = frames.FindAnimation(animationName))
            return named;
    }
    return frames.Animations().empty() ? nullptr : &frames.Animations().front();
}

/** Decodes the chosen frame's sheet and crops it. Empty when anything is missing. */
ThumbnailImage RenderPreview(const std::filesystem::path& assetPath,
                             const std::string& animationName, bool flipX,
                             AssetManager* assetManager, const std::filesystem::path& assetsRoot)
{
    Sprite2D::SpriteFrames frames(GUID::Null(), assetPath);
    if (!frames.Load())
        return {};

    const Sprite2D::SpriteFrameAnimation* animation = PickAnimation(frames, animationName);
    if (animation == nullptr || animation->Frames.empty())
        return {};

    const Sprite2D::SpriteFrame& frame = animation->Frames.front();
    const ThumbnailImage sheet =
        DecodeImage(ResolveAssetPathByGuid(assetManager, frame.TextureGuid, assetsRoot));

    // Frame V is image-space (V0 = top), which is also the crop's row order.
    const Editor::ThumbnailCropRect frameRect{frame.U0, frame.V0, frame.U1, frame.V1};
    const Editor::ThumbnailCropRect artRect =
        Editor::TrimToOpaqueBounds(sheet, frameRect, kOpaqueAlphaThreshold);

    ThumbnailImage preview = Editor::CropNearest(sheet, artRect, kMaxEdgePx);
    preview = Editor::ScaleUpInteger(preview, kMinEdgePx, kMaxEdgePx);

    const int32 longEdge = std::max(preview.Width, preview.Height);
    preview = Editor::AddTransparentMargin(
        preview, static_cast<int32>(std::lround(longEdge * kMarginFraction)));

    if (flipX)
        MirrorHorizontal(preview);
    return preview;
}

} // namespace

SpriteFramesThumbnailHandler::SpriteFramesThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem,
                                               AssetManager* assetManager)
    : m_JobSystem(jobSystem), m_AssetManager(assetManager)
{
}

void SpriteFramesThumbnailHandler::ResetForProjectSwitch()
{
    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        m_Dependencies.clear();
    }
    // A running crop holds its own copy of the paths it was given and finishes
    // against the old cache root; dropping the entries here means its result is
    // never claimed by a request from the next project.
    std::lock_guard<std::mutex> lock(m_State->Mutex);
    m_State->InFlight.clear();
    m_State->Failed.clear();
}

std::vector<std::filesystem::path> SpriteFramesThumbnailHandler::DependenciesOf(
    const std::filesystem::path& assetPath) const
{
    const std::string key = assetPath.generic_string();
    const std::filesystem::file_time_type sourceTime = Editor::WriteTimeOrDefault(assetPath);
    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        const auto it = m_Dependencies.find(key);
        if (it != m_Dependencies.end() && it->second.SourceWriteTime == sourceTime)
            return it->second.Dependencies;
    }

    // Frames of one asset may come from different sheets, and any of them
    // re-exported has to invalidate the crop taken out of it.
    std::vector<std::filesystem::path> dependencies;
    std::unordered_set<GUID> seen;
    Sprite2D::SpriteFrames frames(GUID::Null(), assetPath);
    if (frames.Load())
    {
        for (const Sprite2D::SpriteFrameAnimation& animation : frames.Animations())
        {
            for (const Sprite2D::SpriteFrame& frame : animation.Frames)
            {
                if (!seen.insert(frame.TextureGuid).second)
                    continue;
                dependencies.push_back(
                    ResolveAssetPathByGuid(m_AssetManager, frame.TextureGuid, m_AssetsRoot));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        m_Dependencies[key] = DependencyEntry{sourceTime, dependencies};
    }
    return dependencies;
}

std::filesystem::path SpriteFramesThumbnailHandler::CachePathFor(
    const std::filesystem::path& assetPath, const std::string& animationName, bool flipX) const
{
    std::error_code ec;
    if (!std::filesystem::exists(assetPath, ec))
        return {};

    std::size_t stamp = 0;
    HashCombine(stamp, static_cast<std::size_t>(kPreviewFormatVersion));
    HashPathAndWriteTime(stamp, assetPath);
    for (const std::filesystem::path& dependency : DependenciesOf(assetPath))
        HashPathAndWriteTime(stamp, dependency);

    const std::string fileName = CacheIdentityPrefix(assetPath, animationName, flipX) +
                                 std::to_string(stamp) + ".png";
    if (!m_CacheRoot.empty())
        return m_CacheRoot / fileName;
    return assetPath.parent_path() / ("." + fileName);
}

std::string SpriteFramesThumbnailHandler::GetOrRequest(
    const std::filesystem::path& assetPath, int /*desiredSize*/,
    std::function<void(const std::string& relPath)> onReady, bool /*StaticModelListThumbnail*/)
{
    return GetOrRequestFrame(assetPath, std::string(), false, std::move(onReady));
}

std::string SpriteFramesThumbnailHandler::GetOrRequestFrame(
    const std::filesystem::path& assetPath, const std::string& animationName, bool flipX,
    std::function<void(const std::string& relPath)> onReady)
{
    const std::filesystem::path cachePath = CachePathFor(assetPath, animationName, flipX);
    if (cachePath.empty())
    {
        if (onReady)
            onReady(std::string());
        return {};
    }

    std::error_code ec;
    if (std::filesystem::exists(cachePath, ec))
    {
        const std::string result = cachePath.generic_string();
        if (onReady)
            onReady(result);
        return result;
    }

    const std::string key = cachePath.generic_string();
    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        if (m_State->Failed.count(key) != 0)
        {
            if (onReady)
                onReady(std::string());
            return {};
        }
        if (!m_State->InFlight.insert(key).second)
            return {};
    }

    // Decoding a sheet is far too much for the UI thread, and a hierarchy of
    // sprites asks for every visible row at once.
    AssetManager* assetManager = m_AssetManager;
    const std::filesystem::path assetsRoot = m_AssetsRoot;
    const std::filesystem::path cacheRoot = m_CacheRoot;
    const std::string identityPrefix = CacheIdentityPrefix(assetPath, animationName, flipX);
    m_JobSystem.EnqueueWork([state = m_State, assetPath, animationName, flipX, cachePath, key, assetManager,
                 assetsRoot, cacheRoot, identityPrefix, onReady]() {
        const ThumbnailImage preview =
            RenderPreview(assetPath, animationName, flipX, assetManager, assetsRoot);

        const bool wrote = WritePng(cachePath, preview);
        if (wrote)
            RemoveStaleCacheFiles(cacheRoot, identityPrefix, cachePath);
        else
        {
            // An asset with no animations, a frame whose sheet is missing, or a
            // sheet that failed to decode: report nothing and let the service
            // fall back to the same icon every other unpreviewable asset gets.
            Logger::Log::Info("SpriteFramesThumbnailHandler: no preview for '{}'",
                              assetPath.string());
        }

        {
            std::lock_guard<std::mutex> lock(state->Mutex);
            state->InFlight.erase(key);
            if (!wrote)
                state->Failed.insert(key);
        }

        if (onReady)
            onReady(wrote ? cachePath.generic_string() : std::string());
    }, JobSystem::JobPriority::Background);

    return {};
}

std::string SpriteFramesThumbnailHandler::GetOrRequestAtlasRegion(
    const std::filesystem::path& texturePath, const std::array<float, 4>& uv,
    std::function<void(const std::string&)> onReady, const std::array<float, 2>& magentaKey)
{
    for (float v : uv) if (!std::isfinite(v)) return {};
    std::size_t stamp = 5;
    for (float v : magentaKey) { if (!std::isfinite(v)) return {}; HashCombine(stamp, std::hash<float>{}(v)); }
    HashPathAndWriteTime(stamp, texturePath);
    for (float v : uv) HashCombine(stamp, std::hash<float>{}(v));
    const auto cachePath = m_CacheRoot / ("atlasthumb_" + std::to_string(stamp) + ".png");
    std::error_code ec;
    if (std::filesystem::exists(cachePath, ec)) return cachePath.generic_string();
    const std::string key = cachePath.generic_string();
    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        if (m_State->Failed.count(key) || !m_State->InFlight.insert(key).second) return {};
    }
    m_JobSystem.EnqueueWork([state=m_State, texturePath, uv, magentaKey, cachePath, key, onReady]() {
        auto sheet = DecodeImage(texturePath);
        // Opt-in material preview contract: use the authored shader's magenta
        // excess threshold, never infer a key from the image's colors.
        if (magentaKey[0] >= 0.0f)
            for (size_t i=0; i+3<sheet.Pixels.size(); i+=4) {
                const float excess = (float(std::min(sheet.Pixels[i], sheet.Pixels[i+2])) - sheet.Pixels[i+1]) / 255.0f;
                if (excess > magentaKey[0]) {
                    // Clear hidden RGB too: thumbnail filtering must not bleed
                    // the removed matte into neighboring translucent texels.
                    sheet.Pixels[i]=sheet.Pixels[i+1]=sheet.Pixels[i+2]=sheet.Pixels[i+3]=0;
                }
                if (magentaKey[1] > 0.0f) sheet.Pixels[i] = std::min(sheet.Pixels[i], sheet.Pixels[i+1]);
            }
        const Editor::ThumbnailCropRect rect{std::min(uv[0],uv[2]), std::min(uv[1],uv[3]),
                                             std::max(uv[0],uv[2]), std::max(uv[1],uv[3])};
        auto preview = Editor::CropNearest(sheet, Editor::TrimToOpaqueBounds(sheet, rect, 8), 256);
        if (uv[0] > uv[2]) MirrorHorizontal(preview);
        if (uv[1] > uv[3])
            for (int y=0; y<preview.Height/2; ++y)
                for (int x=0; x<preview.Width*4; ++x)
                    std::swap(preview.Pixels[y*preview.Width*4+x],
                              preview.Pixels[(preview.Height-1-y)*preview.Width*4+x]);
        preview = Editor::ScaleUpInteger(preview, 128, 256);
        preview = Editor::AddTransparentMargin(preview, 8);
        const bool wrote = WritePng(cachePath, preview);
        {
            std::lock_guard<std::mutex> lock(state->Mutex);
            state->InFlight.erase(key);
            if (!wrote) state->Failed.insert(key);
        }
        if (onReady) onReady(wrote ? key : std::string());
    }, JobSystem::JobPriority::Background);
    return {};
}

} // namespace GameEngine
