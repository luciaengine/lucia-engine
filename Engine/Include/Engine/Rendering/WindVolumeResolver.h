#pragma once

#include "Types/Types.h"
#include "Rendering/Core/WindVolumeGPU.h"
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Engine::Renderer
{

struct ResolvedWind
{
    float32 VelocityX = 0.0f;
    float32 VelocityY = 0.0f;
    float32 VelocityZ = 0.0f;
    float32 Turbulence = 0.0f;
    float32 GustFrequency = 0.5f;
    float32 GustScale = 70.0f;
    float32 Weight = 0.0f;
    float32 RainAmount = 1.0f;
};

// std140 mirror of Includes/weather_wind_fields.glsl. The highest-priority
// 32 enabled volumes are retained in the same deterministic order as ResolveAt.
inline constexpr uint32 kMaxWeatherWindVolumes = 32;
struct alignas(16) WeatherWindVolumeGPU
{
    float32 CenterBlend[4]{};
    float32 AxisXExtent[4]{};
    float32 AxisYExtent[4]{};
    float32 AxisZExtent[4]{};
    float32 VelocityWeight[4]{};
    float32 Gust[4]{}; // turbulence, frequency, scale, global
    float32 Mode[4]{}; // shape, blend mode, rain amount, reserved
};
struct alignas(16) WeatherWindGPU
{
    // x=volume count, y=water drop size, z=ripple speed, w=reserved.
    float32 Count[4]{0.0f, 1.0f, 1.0f, 0.0f};
    WeatherWindVolumeGPU Volumes[kMaxWeatherWindVolumes]{};
};
static_assert(sizeof(WeatherWindVolumeGPU) == 112);
static_assert(sizeof(WeatherWindGPU) == 3600);

class WindVolumeResolver
{
public:
    static WeatherWindGPU ExtractForGpu(ECS::World& world, uint32 layerMask = 0xFFFFFFFFu);
    static ResolvedWind ResolveAt(ECS::World& world,
                                  float32 worldX,
                                  float32 worldY,
                                  float32 worldZ,
                                  const ResolvedWind& baseWind = {},
                                  uint32 layerMask = 0xFFFFFFFFu);

    // Snapshot enabled volumes for spatial GPU consumers into `out` (cleared first),
    // using the CPU resolver's transforms, layer filtering and deterministic priority
    // order. Callers own `out` so a per-frame extraction reuses its capacity.
    static void ExtractGPU(ECS::World& world, std::vector<Rendering::WindVolumeGPU>& out,
                           uint32 layerMask = 0xFFFFFFFFu);

    static uint64 QuantizedHash(const ResolvedWind& wind);
};

} // namespace GameEngine::Engine::Renderer
