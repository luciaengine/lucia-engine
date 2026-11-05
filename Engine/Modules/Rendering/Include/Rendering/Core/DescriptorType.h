#pragma once

#include <cstdint>

namespace GameEngine::Rendering
{
// Descriptor type enumeration.
//
// Append only: MetalShaderTranslator's on-disk MSL cache hashes the raw
// numeric value of every binding's type, so renumbering silently invalidates
// nothing and reuses stale translations.
enum class DescriptorType : int32_t
{
    UniformBuffer,
    StorageBuffer,
    Texture,
    Sampler,
    CombinedImageSampler,
    StorageImage,
    // Ray-query scene TLAS (GLSL accelerationStructureEXT / MSL
    // raytracing::acceleration_structure). Only meaningful on a device whose
    // RenderingDeviceCapabilities::supportsRayQuery is set — a layout
    // containing one cannot be created otherwise.
    AccelerationStructure
};
} // namespace GameEngine::Rendering
