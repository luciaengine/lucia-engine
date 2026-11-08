#pragma once

#include "Rendering/Materials/ShaderCompileTarget.h"

namespace GameEngine
{
namespace Rendering
{

struct RendererProfile;

// Device defaults for new requests. Queued work retains its ShaderCompileTarget;
// changing these defaults does not change an existing request or cache key.
bool IsCompatShaderProfile();
void SetCompatShaderProfile(bool compat);

// Whether the device enabled the interpolation functions (interpolateAtOffset and its kin: the
// SPIR-V InterpolationFunction capability, which Vulkan gates on sampleRateShading). Device-wide
// like the compat profile, and set at the same point from the resolved RendererProfile. Off until
// a device says otherwise, so a shader composed without one never declares the capability.
bool AreInterpolationFunctionsAvailable();
void SetInterpolationFunctionsAvailable(bool available);

// Sets every shader compile input a renderer profile decides (the compat profile, the interpolation
// functions) together. The runtime passes the profile its device resolved; an offline cook passes the
// profile of the device class it cooks for, so each cooked program carries the defines that class's
// runtime composes and the runtime finds it under the same cache key.
void ApplyShaderCompileProfile(const RendererProfile& profile);
void ApplyShaderCompileProfile(const ShaderCompileTarget& target);

} // namespace Rendering
} // namespace GameEngine
