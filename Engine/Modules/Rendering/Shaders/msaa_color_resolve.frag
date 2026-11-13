#version 450

// Resolves the multisampled scene color into its single-sample target with the display-weighted mean
// (Includes/msaa_display_resolve.glsl) under the view's look and exposure. MsaaColorResolvePass.cpp
// declares it after every pass that draws into the multisampled scene color.

layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2DMS uSamples;

#include "Includes/msaa_display_resolve.glsl"
#include "Includes/view_exposure.glsl"

void main()
{
    vec4 plain;
    GE_MsaaDisplayResolve(uSamples, ivec2(gl_FragCoord.xy), int(ge_exposureParams.z),
                          GE_ViewExposureScale() / GE_PreExposure(), oColor, plain);
}
