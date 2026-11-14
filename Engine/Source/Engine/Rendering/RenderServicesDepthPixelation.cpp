#include "Engine/Rendering/RenderServices.h"

#include "Mathematics/Matrix4x4.h"
#include "Engine/Rendering/RenderOrigin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Engine::Renderer
{
std::array<uint32_t, 2> RenderServices::DepthPixelationRasterExtent(
    const PostProcessSettings& pp, int band, uint32_t width, uint32_t height)
{
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (pp.DepthPixelationAutoBandResolution != 0 || pp.DepthPixelationResolutionMode == 1)
    {
        float divisor = pp.GetDepthPixelationBandDivisor(band);
        if (pp.DepthPixelationAutoBandResolution != 0)
        {
            const int count = std::clamp(pp.DepthPixelationBandCount, 1, DepthPixelationViews::kMaxBands);
            const int exponent = band == DepthPixelationViews::kSkyIndex ? 0 : std::max(0, count - 1 - band);
            divisor = static_cast<float>(std::clamp(pp.DepthPixelationAutoFarPixelSize, 1, 2) * (1 << exponent));
        }
        divisor = std::isfinite(divisor) ? std::max(1.0f, divisor) : 1.0f;
        return {std::max(1u, static_cast<uint32_t>(std::ceil(width / divisor))),
                std::max(1u, static_cast<uint32_t>(std::ceil(height / divisor)))};
    }
    const float requested = pp.GetDepthPixelationBandResolution(band);
    const auto h = static_cast<uint32_t>(std::clamp(std::round(std::isfinite(requested) ? requested : float(height)), 1.0f, float(height)));
    return {std::max(1u, static_cast<uint32_t>(std::round(float(h) * width / height))), h};
}

Rendering::CameraData RenderServices::StabilizeDepthPixelationCamera(
    const Rendering::CameraData& camera, uint32_t width, uint32_t height,
    float focusDistance, bool enabled, std::array<float, 4>& uvTransform, uint32_t guardPixels)
{
    uvTransform = {1, 1, 0, 0};
    if (!enabled || width == 0 || height == 0) return camera;
    auto result = camera;
    const float sx = float(width) / (width + 2 * guardPixels);
    const float sy = float(height) / (height + 2 * guardPixels);
    float shiftX = 0, shiftY = 0;
    if (focusDistance > 0 && std::abs(camera.proj.Data()[0]) > 1e-6f && std::abs(camera.proj.Data()[5]) > 1e-6f)
    {
        const bool perspective = std::abs(camera.proj.Data()[15]) < 1e-6f;
        const double focus = perspective ? std::max(0.01f, focusDistance) : 1.0;
        const double pixelX = 2.0 * focus / (std::abs(camera.proj.Data()[0]) * width);
        const double pixelY = 2.0 * focus / (std::abs(camera.proj.Data()[5]) * height);
        const Mathematics::Vector3F64 translation = camera.renderOriginSector[3] != 0
            ? ComputeRebasedViewTranslation(camera.view, CameraEye(camera), {})
            : Mathematics::Vector3F64{camera.view[3][0], camera.view[3][1], camera.view[3][2]};
        const double x = translation.X / pixelX;
        const double y = translation.Y / pixelY;
        shiftX = static_cast<float>(2.0 * (std::round(x) - x) / width);
        shiftY = static_cast<float>(2.0 * (std::round(y) - y) / height);
    }
    // Snap the projection, leaving gameplay transforms and distance-band
    // measurements untouched. One guard texel on every side provides valid
    // samples when the final image compensates the subpixel snap.
    for (int column = 0; column < 4; ++column)
    {
        result.proj.Data()[column * 4] = sx * (camera.proj.Data()[column * 4] + shiftX * camera.proj.Data()[column * 4 + 3]);
        result.proj.Data()[column * 4 + 1] = sy * (camera.proj.Data()[column * 4 + 1] + shiftY * camera.proj.Data()[column * 4 + 3]);
    }
    result.viewProj = result.proj * result.view;
    result.viewProjRel = result.proj * result.viewRel;
    uvTransform = {sx, sy, 0.5f * shiftX * sx, -0.5f * shiftY * sy};
    return result;
}

namespace
{
float BandFocusDistance(const PostProcessSettings& pp, int band, int count)
{
    if (band == RenderServices::DepthPixelationViews::kSkyIndex) return 0;
    float start = 0;
    for (int i = 0; i < band; ++i) start = std::max(start, pp.GetDepthPixelationBandEnd(i));
    if (band + 1 == count) return std::max(10.0f, start * 1.5f);
    return std::max(0.01f, (start + std::max(start, pp.GetDepthPixelationBandEnd(band))) * 0.5f);
}
}

bool RenderServices::IsDepthPixelationView(Rendering::ViewId view) const
{
    for (const auto& [parent, bands] : m_DepthPixelationViews)
        for (auto child : bands.Views)
            if (child != 0 && child == view) return true;
    return false;
}

const RenderServices::DepthPixelationViews*
RenderServices::FindDepthPixelationViews(Rendering::ViewId parent) const
{
    const auto it = m_DepthPixelationViews.find(parent);
    return it == m_DepthPixelationViews.end() ? nullptr : &it->second;
}

bool RenderServices::GetDepthPixelationBandRange(Rendering::ViewId view, float& start,
                                                float& end, int& distanceMode) const
{
    for (const auto& [parentId, bands] : m_DepthPixelationViews)
    {
        if (bands.SimpleScreenSpace) continue; // Full scene image: no per-band clipping.
        const auto* parent = Views().FindViewDesc(parentId);
        if (!parent) continue;
        const auto& pp = GetEffectivePostProcessSettings(parentId, parent->worldId);
        float previous = 0.0f;
        for (int i = 0; i < bands.BandCount; ++i)
        {
            const float next = i + 1 == bands.BandCount ? 1e30f
                              : std::max(previous, pp.GetDepthPixelationBandEnd(i));
            if (bands.Views[i] == view)
            {
                start = previous;
                end = next;
                distanceMode = pp.DepthPixelationDistanceMode;
                return true;
            }
            previous = next;
        }
    }
    return false;
}

void RenderServices::SyncDepthPixelationViews(uint64 worldId, bool allowAllocation)
{
    // Disabled worlds with no owned views need neither a snapshot nor allocation.
    // Existing views still enter the cleanup path when the effect turns off.
    if (m_DepthPixelationViews.empty())
    {
        const auto& views = Views().GetViews();
        const bool enabled = std::any_of(views.begin(), views.end(), [&](const auto& view) {
            if (!view.activeRenderPipeline || view.ActiveRenderLayerMask().Bits == 0 ||
                (view.worldId != 0 && view.worldId != worldId))
                return false;
            const auto& pp = GetEffectivePostProcessSettings(view.id, worldId);
            return pp.DepthPixelationIntensity > 0 || pp.DepthPixelationDebugRanges != 0;
        });
        if (!enabled) return;
    }
    // Copy descriptors: allocating/releasing views invalidates the registry vector.
    const auto parents = Views().GetViews();
    if (allowAllocation)
    {
        for (auto it = m_DepthPixelationViews.begin(); it != m_DepthPixelationViews.end();)
        {
            const auto* parent = Views().FindViewDesc(it->first);
            bool remove = !parent;
            if (parent && (parent->worldId == worldId || parent->worldId == 0))
            {
                const auto& pp = GetEffectivePostProcessSettings(parent->id, worldId);
                remove = !parent->activeRenderPipeline || parent->ActiveRenderLayerMask().Bits == 0 ||
                         (pp.DepthPixelationIntensity <= 0 && pp.DepthPixelationDebugRanges == 0) ||
                         it->second.SimpleScreenSpace != (pp.DepthPixelationSimpleScreenSpace != 0) ||
                         it->second.BandCount != (pp.DepthPixelationSimpleScreenSpace != 0 ? 1 :
                             std::clamp(pp.DepthPixelationBandCount, 1, DepthPixelationViews::kMaxBands));
            }
            if (!remove) { ++it; continue; }
            for (int i = 0; i <= DepthPixelationViews::kMaxBands; ++i)
            {
                if (it->second.Views[i] == 0) continue;
                Views().ReleaseView(it->second.Views[i]);
                Views().ReleaseCamera(it->second.Cameras[i]);
            }
            it = m_DepthPixelationViews.erase(it);
        }
    }
    for (const auto& parent : parents)
    {
        if (IsDepthPixelationView(parent.id) || !Views().FindViewDesc(parent.id) ||
            !parent.activeRenderPipeline || parent.ActiveRenderLayerMask().Bits == 0 ||
            (parent.worldId != 0 && parent.worldId != worldId)) continue;
        auto pp = GetEffectivePostProcessSettings(parent.id, worldId);
        if (pp.DepthPixelationIntensity <= 0 && pp.DepthPixelationDebugRanges == 0) continue;
        const auto* cameraPtr = Views().FindCameraData(parent.cameraId);
        if (!cameraPtr) continue;
        const auto camera = *cameraPtr;
        auto it = m_DepthPixelationViews.find(parent.id);
        if (it == m_DepthPixelationViews.end())
        {
            if (!allowAllocation) continue;
            DepthPixelationViews bands;
            bands.SimpleScreenSpace = pp.DepthPixelationSimpleScreenSpace != 0;
            bands.BandCount = bands.SimpleScreenSpace ? 1 : std::clamp(pp.DepthPixelationBandCount, 1, DepthPixelationViews::kMaxBands);
            static constexpr const char* names[] = {
                "Pixelation.Band1", "Pixelation.Band2", "Pixelation.Band3", "Pixelation.Band4", "Pixelation.Band5",
                "Pixelation.Band6", "Pixelation.Band7", "Pixelation.Band8", "Pixelation.Band9", "Pixelation.Band10", "Pixelation.Sky"
            };
            for (int i = 0; i <= DepthPixelationViews::kMaxBands; ++i)
            {
                if (bands.SimpleScreenSpace ? i != 0 : (i >= bands.BandCount && i != DepthPixelationViews::kSkyIndex)) continue;
                const char* name = bands.SimpleScreenSpace ? "Pixelation.Source" : names[i];
                bands.Cameras[i] = Views().AllocateCamera(name);
                bands.Views[i] = Views().AllocateView(name, bands.Cameras[i], Rendering::ViewPurpose::UtilityCapture);
            }
            it = m_DepthPixelationViews.emplace(parent.id, bands).first;
        }
        pp.DepthPixelationIntensity = 0;
        pp.DepthPixelationDebugRanges = 0;
        float previousEnd = 0.0f;
        for (int i = 0; i <= DepthPixelationViews::kMaxBands; ++i)
        {
            const auto child = it->second.Views[i];
            if (child == 0) continue;
            bool empty = false;
            if (i + 1 < it->second.BandCount)
            {
                const float end = std::max(previousEnd, pp.GetDepthPixelationBandEnd(i));
                empty = end <= previousEnd;
                previousEnd = end;
            }
            // Keep the complete projection. The fragment adapter clips by distance
            // with a pixel-footprint guard at band boundaries, retaining background
            // behind rejected foreground geometry and preventing seam holes.
            const auto extent = it->second.RasterExtents[i];
            const auto stabilized = StabilizeDepthPixelationCamera(camera, extent[0], extent[1],
                BandFocusDistance(pp, i, it->second.BandCount), pp.DepthPixelationStabilizationMode != 0,
                it->second.UVTransforms[i], it->second.GuardPixels);
            Views().SetCameraData(it->second.Cameras[i], stabilized, Views().FindCameraRanges(parent.cameraId));
            Views().SetViewWorldId(child, worldId);
            Views().SetViewDropsEditorHidden(child, parent.DropsEditorHidden());
            Views().SetViewWorldPassFlipY(child, Views().GetViewWorldPassFlipY(parent.id));
            Views().SetViewRenderLayerMask(child, parent.renderLayerMask);
            Views().SetViewBackgroundOnly(child, !it->second.SimpleScreenSpace && (i == DepthPixelationViews::kSkyIndex || empty));
            Views().SetViewClearConfig(child, Rendering::ViewClearConfig(true, ColorLinear(0, 0, 0, 0), true, 0));
            Views().SetViewRenderScale(child, 1.0f);
            Views().SetViewAntiAliasing(child, false, AntiAliasingMode::Off, 1);
            Views().SetViewPostProcessOverride(child, pp);
        }
    }
}

void RenderServices::PrepareDepthPixelationFrame(Rendering::RenderGraph::RGFrame& frame,
                                                 std::span<const Pipeline::ViewTargetsRG> parents,
                                                 std::vector<Pipeline::ViewTargetsRG>& targets)
{
    if (m_DepthPixelationViews.empty()) return;
    if (targets.empty()) targets.assign(parents.begin(), parents.end());
    // Editor cameras may move after extraction. Refresh existing band cameras
    // before culling, without allocating or changing the registry's view set.
    std::vector<uint64_t> worlds;
    for (const auto& [parentId, bands] : m_DepthPixelationViews)
        if (const auto* parent = Views().FindViewDesc(parentId))
            if (std::find(worlds.begin(), worlds.end(), parent->worldId) == worlds.end())
                worlds.push_back(parent->worldId);
    for (auto worldId : worlds)
        SyncDepthPixelationViews(worldId, false);
    AppendDepthPixelationTargets(frame, targets);
}

void RenderServices::AppendDepthPixelationTargets(Rendering::RenderGraph::RGFrame& frame,
                                                  std::vector<Pipeline::ViewTargetsRG>& targets)
{
    const size_t parentCount = targets.size();
    for (size_t index = 0; index < parentCount; ++index)
    {
        const auto parentTarget = targets[index];
        auto bandIt = m_DepthPixelationViews.find(parentTarget.View);
        auto* bands = bandIt == m_DepthPixelationViews.end() ? nullptr : &bandIt->second;
        const auto* parent = Views().FindViewDesc(parentTarget.View);
        if (!bands || !parent || !parentTarget.Color.IsValid()) continue;
        const auto& pp = GetEffectivePostProcessSettings(parent->id, parent->worldId);
        if (pp.DepthPixelationIntensity <= 0 && pp.DepthPixelationDebugRanges == 0) continue;
        const auto desc = frame.Graph().ResourceDesc(parentTarget.Color.Id);
        bands->GuardPixels = 1;
        for (int i = 0; i <= DepthPixelationViews::kMaxBands; ++i)
        {
            if (bands->Views[i] == 0) continue;
            const auto extent = bands->SimpleScreenSpace ? std::array<uint32_t, 2>{desc.Width, desc.Height}
                                                        : DepthPixelationRasterExtent(pp, i, desc.Width, desc.Height);
            bands->RasterExtents[i] = extent;
            const auto* camera = Views().FindCameraData(parent->cameraId);
            if (camera)
                Views().SetCameraData(bands->Cameras[i], StabilizeDepthPixelationCamera(*camera,
                    extent[0], extent[1], BandFocusDistance(pp, i, bands->BandCount),
                    pp.DepthPixelationStabilizationMode != 0, bands->UVTransforms[i], bands->GuardPixels),
                    Views().FindCameraRanges(parent->cameraId));
            WriteViewLightBuffer(bands->Views[i]);
            Rendering::TextureDesc td{};
            const uint32_t guard = pp.DepthPixelationStabilizationMode != 0 ? 2u * bands->GuardPixels : 0u;
            td.width = extent[0] + guard;
            td.height = extent[1] + guard;
            td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
            td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                       static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
            const std::string name = "DepthPixelation.View" + std::to_string(bands->Views[i]);
            auto color = frame.ImportPersistentTexture((name + ".Color").c_str(), td);
            auto resolve = frame.ImportPersistentTexture((name + ".Resolve").c_str(), td);
            td.format = parentTarget.Depth.IsValid()
                            ? frame.Graph().ResourceDesc(parentTarget.Depth.Id).Format
                            : static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
            td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                       static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
            auto depth = frame.ImportPersistentTexture((name + ".Depth").c_str(), td);
            targets.push_back({bands->Views[i], color, depth, resolve});
        }
    }
}
} // namespace GameEngine::Engine::Renderer
