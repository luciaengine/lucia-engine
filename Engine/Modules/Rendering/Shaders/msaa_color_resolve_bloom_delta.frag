#version 450

// Resolves the multisampled scene color like msaa_color_resolve.frag, and writes a second output:
// the plain sample mean minus the weighted mean. Bloom adds it back, so its input is the plain mean.

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oBloomSampleDelta;
layout(set = 0, binding = 0) uniform sampler2DMS uSamples;

#include "Includes/msaa_display_resolve.glsl"
#include "Includes/view_exposure.glsl"

void main()
{
    vec4 plain;
    GE_MsaaDisplayResolve(uSamples, ivec2(gl_FragCoord.xy), int(ge_exposureParams.z),
                          GE_ViewExposureScale() / GE_PreExposure(), oColor, plain);
    oBloomSampleDelta = vec4(plain.rgb - oColor.rgb, 0.0);
}
