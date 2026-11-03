#include "Engine/Rendering/ViewExposureScale.h"

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraTypes.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

namespace
{
constexpr float32 kMinExposureScale = 1.0e-4f;
constexpr float32 kMaxExposureScale = 1.0e4f;
} // namespace

float32 ResolveViewExposureScale(RenderServices& renderServices, const Rendering::ViewDesc& view)
{
    const PostProcessSettings& settings = renderServices.GetEffectivePostProcessSettings(view.id, view.worldId);
    float32 exposureScale = settings.Exposure;
    if (settings.IsAutoExposureActive())
    {
        auto& readback = renderServices.EnsureFeature<ExposureReadbackFeature>();
        readback.SetReadbackEnabled(view.id, true);
        float32 adapted = 0.0f;
        if (readback.TryResolveAdaptedExposure(view.id, adapted))
            exposureScale = adapted;
    }
    return std::clamp(exposureScale, kMinExposureScale, kMaxExposureScale);
}

} // namespace GameEngine::Engine::Renderer
