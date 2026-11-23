#version 450
#include "Includes/foliage_coverage.glsl"
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Probe { float lod; float cutoff; vec2 phase; uint mode; float seed; } probe;
float Foliage(vec4 texel,float layerSeed)
{
    // The weight sees the UV per pixel at which a sampler reads level probe.lod; the threshold sees
    // the quad's own UV per pixel, as in a draw.
    float uvPerPixel=exp2(probe.lod)/float(textureSize(sourceImage,0).x);
    float weight=GE_FoliageTailWeight(vec2(uvPerPixel,0.0),vec2(0.0,uvPerPixel),0.0);
    float threshold=GE_FoliageSurfaceThreshold(uv+probe.phase,dFdx(uv),dFdy(uv),layerSeed);
    return GE_FoliageCoverageOpacity(texel.a,probe.cutoff,weight,threshold);
}
void main()
{
    if(probe.mode==0u)
    {
        result=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),int(probe.lod));
        return;
    }
    vec4 texel=textureLod(sourceImage,uv+probe.phase,probe.lod);
    // Mode 1: the same inclusive cutoff boundary as the production forward/depth adapters.
    // Mode 2: the production foliage coverage test of a leaf texture.
    // Mode 3: two overlapping leaves in one pixel, seeds 0 and probe.seed.
    float opacity=texel.a;
    if(probe.mode==2u)
        opacity=Foliage(texel,0.0);
    else if(probe.mode==3u)
        opacity=max(Foliage(texel,0.0),Foliage(texel,probe.seed));
    if(opacity<probe.cutoff) discard;
    result=vec4(1.0);
}
