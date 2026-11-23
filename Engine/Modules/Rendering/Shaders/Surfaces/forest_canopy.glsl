// The overview canopy of a packed forest. The forest cooker aligns and pads each page's atlas
// tile through mip 3, so the filtered alpha stays inside the tile and becomes coverage at distance.
// @texture albedoMap srgb
#include "Includes/foliage_coverage.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec2 texelScale = vec2(textureSize(albedoMap, 0));
    vec2 dx = dFdx(sIn.uv0) * texelScale;
    vec2 dy = dFdy(sIn.uv0) * texelScale;
    float mip = 0.5 * log2(max(1.0, max(dot(dx, dx), dot(dy, dy))));
    vec4 canopy = textureLod(albedoMap, sIn.uv0, min(mip, 3.0));
    float alpha = canopy.a * Mat.uBaseColor.a;
    float cutout = step(clamp(Props.alphaCutoff, 0.001, 1.0), alpha);
    float coverage = mix(cutout, alpha, smoothstep(0.5, 2.0, mip));
    // Use the same screen-space threshold for every pass at this pixel and pose.
    float threshold = GE_FoliageScreenThreshold(gl_FragCoord.xy);
    o.opacity = coverage > threshold ? 1.0 : 0.0;
#if !defined(GE_DEPTH_ONLY_FRAGMENT) && !defined(GE_MOTION_VECTORS)
    o.normalWS = normalize(sIn.normalWS);
    o.roughness = 1.0;
    o.metallic = 0.0;
    o.baseColor = canopy.rgb * Mat.uBaseColor.rgb;
#endif
    return o;
}
