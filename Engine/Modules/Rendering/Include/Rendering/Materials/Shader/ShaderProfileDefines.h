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

// Whether the device enabled the interpolation functions (interpolateAt*, SPIR-V InterpolationFunction): Vulkan gates
// them on sampleRateShading, Metal on pull-model interpolation. Device-wide, set with the compat profile from the
// resolved RendererProfile. Off until a device sets it, so a shader composed without one never declares the capability.
bool AreInterpolationFunctionsAvailable();
void SetInterpolationFunctionsAvailable(bool available);

// Whether the device enabled clip distances (gl_ClipDistance: the SPIR-V ClipDistance capability,
// which Vulkan gates on shaderClipDistance and Metal always has). Device-wide, set like the
// interpolation functions, off until a device says otherwise.
bool AreClipDistancesAvailable();
void SetClipDistancesAvailable(bool available);

// Sets every shader compile input a renderer profile decides (the compat profile, the interpolation
// functions, the clip distances) together. The runtime passes the profile its device resolved; an
// offline cook passes the profile of the device class it cooks for, so each cooked program carries the
// defines that class's runtime composes and the runtime finds it under the same cache key.
void ApplyShaderCompileProfile(const RendererProfile& profile);
void ApplyShaderCompileProfile(const ShaderCompileTarget& target);

} // namespace Rendering
} // namespace GameEngine
