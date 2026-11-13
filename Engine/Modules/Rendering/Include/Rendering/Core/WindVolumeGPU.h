#pragma once
#include "Mathematics/Vector4.h"

namespace GameEngine::Rendering
{
// Shared std430 wind-volume record, sorted by priority/entity.
struct alignas(16) WindVolumeGPU
{
    Mathematics::Vector4 CenterShape{}; // xyz center; w: -1 global, -2 invalid, otherwise WindVolumeShape
    Mathematics::Vector4 AxisXExtent{};
    Mathematics::Vector4 AxisYExtent{};
    Mathematics::Vector4 AxisZExtent{};
    Mathematics::Vector4 VelocityWeight{};
    Mathematics::Vector4 Gust{}; // turbulence, frequency, wavelength, blend distance
    Mathematics::Vector4 Mode{}; // x: 0 additive, 1 override
};
static_assert(sizeof(WindVolumeGPU) == 112);
}
