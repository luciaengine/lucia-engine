// Shadow coverage for a light-facing impostor. The vertex modifier faces the
// cascade camera. Reproject its local card point into the nearest atlas view.
// @texture albedoMap srgb
// @property vec4 impostorAtlas "Impostor Atlas" default=8,0,0,0 group=Impostor

#include "Includes/impostor_frame.glsl"
#include "Includes/foliage_coverage.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(GE_ImpostorDecode(sIn.custom0.xy, Props.impostorAtlas), right, up);
    vec2 corner = vec2(sIn.uv0.x * 2.0 - 1.0, 1.0 - sIn.uv0.y * 2.0);
    vec3 offset = right * corner.x + up * corner.y;

    float frameCount = max(Props.impostorAtlas.x, 1.0);
    vec2 tile = clamp(floor(sIn.custom0.xy * (frameCount - 1.0) + 0.5), vec2(0.0), vec2(frameCount - 1.0));
    GE_ImpostorFrameBasis basis = GE_ImpostorBuildFrameBasis(tile, Props.impostorAtlas);
    vec2 tileTexels = vec2(textureSize(albedoMap, 0)) / frameCount;
    vec2 dx = dFdx(sIn.uv0) * tileTexels;
    vec2 dy = dFdy(sIn.uv0) * tileTexels;
    float footprint = max(dot(dx, dx), dot(dy, dy));
    float mip = min(max(0.0, Props.impostorAtlas.w), 0.5 * log2(max(1.0, footprint)));
    vec2 inset = min(vec2(0.5), 0.5 * exp2(ceil(mip)) / tileTexels);
    vec2 uv = GE_ImpostorFrameUv(tile, basis, offset, Props.impostorAtlas, inset);

    o.opacity = textureLod(albedoMap, uv, mip).a;
    if (mip > 0.5)
    {
        float cutout = step(clamp(Props.alphaCutoff, 0.0, 1.0), o.opacity);
        float coverage = mix(cutout, o.opacity, smoothstep(0.5, 2.0, mip));
        o.opacity = coverage > GE_FoliageScreenThreshold(gl_FragCoord.xy) ? 1.0 : 0.0;
    }
    return o;
}
