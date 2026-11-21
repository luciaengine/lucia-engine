#include "Assets/HlodSelect.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Hlod {

SwitchConfig MakeSwitchConfig(float switchCoverage, float switchHysteresis) {
    SwitchConfig config;
    config.EnterCoverage = std::max(switchCoverage, 0.0f);
    const float band = std::max(switchHysteresis, 0.0f);
    config.ExitCoverage = config.EnterCoverage * (1.0f + band);
    return config;
}

double ClusterDistance(const ViewLod& view, const float sphereCenter[3]) {
    const double dx = view.CameraPos[0] - sphereCenter[0];
    const double dy = view.CameraPos[1] - sphereCenter[1];
    const double dz = view.CameraPos[2] - sphereCenter[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float ClusterCoverage(const ViewLod& view, const float sphereCenter[3], float sphereRadius) {
    if (view.ProjScaleY <= 0.0f)
        return kAlwaysMembersCoverage; // LOD disabled (ortho/shadow) -> keep members

    return SphereCoverageAtDistance(ClusterDistance(view, sphereCenter), sphereRadius,
                                    view.ProjScaleY, std::exp2(view.LodBiasGlobal));
}

float SphereCoverageAtDistance(double distance, float sphereRadius, float projectionScale, float biasScale)
{
    constexpr double kMinimumDistance = 1.0e-4;
    const float clampedDistance = static_cast<float>(std::max(distance, kMinimumDistance));
    return (sphereRadius * projectionScale / clampedDistance) * biasScale;
}

bool UpdateProxyActive(bool wasProxyActive, float coverage, const SwitchConfig& config) {
    if (wasProxyActive) {
        // Stay proxy until coverage rises past the (larger) exit threshold.
        return coverage < config.ExitCoverage;
    }
    // Engage proxy only once coverage drops below the enter threshold.
    return coverage < config.EnterCoverage;
}

} // namespace Hlod
} // namespace GameEngine
