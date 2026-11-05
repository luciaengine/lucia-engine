#pragma once

#include <cstdint>

namespace GameEngine::Rendering
{
/// Descriptor types used by device layouts and shader binding plans.
/// Keep numeric values stable for the Metal shader-cache key; append new types.
enum class DescriptorType : int32_t
{
    UniformBuffer,
    StorageBuffer,
    Texture,
    Sampler,
    CombinedImageSampler,
    StorageImage,
    /// Ray-query scene TLAS: GLSL accelerationStructureEXT or MSL raytracing::acceleration_structure.
    /// Layout creation requires RenderingDeviceCapabilities::supportsRayQuery.
    AccelerationStructure
};
} // namespace GameEngine::Rendering
