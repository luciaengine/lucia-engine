#ifndef GE_DDGI_DEPTH_NORMAL_GLSL
#define GE_DDGI_DEPTH_NORMAL_GLSL

// Shared by the resolve, the resolve blur, the resolve upsample and their GPU regression fixtures.

// The raw depth at `uv`. A kernel defines it before this include when it must not sample the depth texture.
#ifndef GE_DDGI_DEPTH_TAP
#define GE_DDGI_DEPTH_TAP(depthTexture, uv) textureLod(depthTexture, uv, 0.0).r
#endif

vec2 GE_DDGIUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

vec3 GE_DDGIWorldPositionAt(vec2 uv, float rawDepth, mat4 invProj, mat4 invView)
{
    vec4 p = invProj * vec4(GE_DDGIUvToNdc(uv), rawDepth, 1.0);
    return (invView * vec4(p.xyz / max(abs(p.w), 1e-7), 1.0)).xyz;
}

vec3 GE_DDGINormalFromDepth(sampler2D depthTexture, vec2 uv, float rawDepth,
                             vec3 posWS, vec3 toCamera, mat4 invProj, mat4 invView)
{
    vec2 dtexel = 1.0 / vec2(textureSize(depthTexture, 0));
    // A clamped depth outside the viewport is not a geometric neighbour.
    // Reconstructing it at the outside UV invents a camera-facing tangent,
    // producing a bright border that temporal reprojection carries inward.
    ivec2 pixel = ivec2(uv * vec2(textureSize(depthTexture, 0)));
    ivec2 size = textureSize(depthTexture, 0);
    bool validR = pixel.x + 1 < size.x;
    bool validL = pixel.x > 0;
    bool validD = pixel.y + 1 < size.y;
    bool validU = pixel.y > 0;
    // Geometric normal from depth differences: per axis, reconstruct both
    // neighbours, and at a silhouette difference against whichever one is
    // closer in depth to the centre so the tangent does not smear across
    // objects. Raw reverse-Z depth is monotonic in view depth, so the
    // |raw - centre| comparison picks the same neighbour linear depth would.
    float dR = GE_DDGI_DEPTH_TAP(depthTexture, uv + vec2(dtexel.x, 0.0));
    float dL = GE_DDGI_DEPTH_TAP(depthTexture, uv - vec2(dtexel.x, 0.0));
    float dD = GE_DDGI_DEPTH_TAP(depthTexture, uv + vec2(0.0, dtexel.y));
    float dU = GE_DDGI_DEPTH_TAP(depthTexture, uv - vec2(0.0, dtexel.y));
    validR = validR && dR > 1e-7;
    validL = validL && dL > 1e-7;
    validD = validD && dD > 1e-7;
    validU = validU && dU > 1e-7;
    if (!(validR || validL) || !(validD || validU))
        return normalize(toCamera);
    // Central difference wherever the two neighbours agree, one-sided only at a
    // silhouette. Picking the closer neighbour unconditionally looks robust but
    // is not: on a smooth surface the two disparities are equal to within noise,
    // so the choice flips from pixel to pixel and the reconstructed normal
    // alternates between two slightly different one-sided fits — a checkerboard
    // that the reflection gather magnifies into facets on curved reflectors.
    vec3 pR = GE_DDGIWorldPositionAt(uv + vec2(dtexel.x, 0.0), dR, invProj, invView);
    vec3 pL = GE_DDGIWorldPositionAt(uv - vec2(dtexel.x, 0.0), dL, invProj, invView);
    vec3 pD = GE_DDGIWorldPositionAt(uv + vec2(0.0, dtexel.y), dD, invProj, invView);
    vec3 pU = GE_DDGIWorldPositionAt(uv - vec2(0.0, dtexel.y), dU, invProj, invView);
    float dxR = abs(dR - rawDepth);
    float dxL = abs(dL - rawDepth);
    float dyD = abs(dD - rawDepth);
    float dyU = abs(dU - rawDepth);
    // Relative test: a silhouette makes one side disagree by orders of
    // magnitude, a smooth surface keeps the pair within this factor.
    const float kEdgeDisparityRatio = 4.0;
    bool smoothX = validR && validL && max(dxR, dxL) <= kEdgeDisparityRatio * min(dxR, dxL) + 1.0e-9;
    bool smoothY = validD && validU && max(dyD, dyU) <= kEdgeDisparityRatio * min(dyD, dyU) + 1.0e-9;
    vec3 ddx = smoothX ? (pR - pL) * 0.5 :
        (validR && (!validL || dxR <= dxL) ? pR - posWS : posWS - pL);
    vec3 ddy = smoothY ? (pD - pU) * 0.5 :
        (validD && (!validU || dyD <= dyU) ? pD - posWS : posWS - pU);
    vec3 N = cross(ddy, ddx);
    float len = length(N);
    if (len <= 1e-12)
        N = normalize(toCamera);  // degenerate (grazing/collapsed): face the camera
    else
        N /= len;
    // Camera-facing regardless of the cross order's handedness.
    if (dot(N, toCamera) < 0.0)
        N = -N;

    return N;
}

// True where the depth at `uv` and its 4 neighbours are not on one plane: a crease or a silhouette is next to the
// texel. On a plane, reverse-Z depth is affine in screen space, so its second difference is 0 on each axis.
bool GE_DDGIDepthCreaseAt(sampler2D depthTexture, vec2 uv, float rawDepth)
{
    if (!(rawDepth > 0.0))
        return false;
    ivec2 size = textureSize(depthTexture, 0);
    vec2 dtexel = 1.0 / vec2(size);
    ivec2 pixel = ivec2(uv * vec2(size));
    // The second difference relative to the slope. A curved surface stays far below this ratio.
    const float kCreaseCurvatureRatio = 0.25;
    // Depth noise of a plane that faces the camera, relative to its depth.
    const float kCreaseNoiseFloor = 1.0e-5;
    bool crease = false;
    if (pixel.x > 0 && pixel.x + 1 < size.x)
    {
        float dR = GE_DDGI_DEPTH_TAP(depthTexture, uv + vec2(dtexel.x, 0.0));
        float dL = GE_DDGI_DEPTH_TAP(depthTexture, uv - vec2(dtexel.x, 0.0));
        float slope = abs(dR - rawDepth) + abs(dL - rawDepth);
        crease = abs(dR + dL - 2.0 * rawDepth) > kCreaseCurvatureRatio * slope + kCreaseNoiseFloor * rawDepth;
    }
    if (pixel.y > 0 && pixel.y + 1 < size.y)
    {
        float dD = GE_DDGI_DEPTH_TAP(depthTexture, uv + vec2(0.0, dtexel.y));
        float dU = GE_DDGI_DEPTH_TAP(depthTexture, uv - vec2(0.0, dtexel.y));
        float slope = abs(dD - rawDepth) + abs(dU - rawDepth);
        crease = crease ||
            abs(dD + dU - 2.0 * rawDepth) > kCreaseCurvatureRatio * slope + kCreaseNoiseFloor * rawDepth;
    }
    return crease;
}

#endif
