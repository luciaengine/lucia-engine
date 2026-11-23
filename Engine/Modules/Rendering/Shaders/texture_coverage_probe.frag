#version 450
#include "Includes/foliage_coverage.glsl"
#include "Includes/integer_hash.glsl"
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Probe { float lod; float cutoff; vec2 phase; uint mode; float seed; } probe;
// Modes 2 to 4 draw a grid of kLeaves x kLeaves leaves. Each leaf is one copy of the image with its
// own seed: the golden-ratio sequence over the leaf index, as the tree generator writes it (UV1.x).
const float kLeaves=16.0;
float LeafSeed(vec2 position,uint layerKey)
{
    uvec2 cell=uvec2(ivec2(floor(position*kLeaves)))&uvec2(15u);
    uint index=cell.y*16u+cell.x;
    // Another layer is another set of leaves: a leaf of it covers a leaf of an unrelated index.
    if(layerKey!=0u)
        index=GE_HashU32(index^layerKey)&0xffffu;
    return fract(float(index)*0.6180339887);
}
float Foliage(vec4 texel,float layerSeed)
{
    // The weight sees the UV per pixel at which a sampler reads level probe.lod of the leaf.
    float uvPerPixel=exp2(probe.lod)/float(textureSize(sourceImage,0).x);
    float weight=GE_FoliageTailWeight(vec2(uvPerPixel,0.0),vec2(0.0,uvPerPixel),0.0);
    return GE_FoliageCoverageOpacity(texel.a,probe.cutoff,weight,layerSeed);
}
void main()
{
    if(probe.mode==0u)
    {
        result=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),int(probe.lod));
        return;
    }
    vec2 position=uv+probe.phase;
    if(probe.mode==1u)
    {
        // Same inclusive boundary as the production forward/depth adapters.
        if(textureLod(sourceImage,position,probe.lod).a<probe.cutoff) discard;
        result=vec4(1.0);
        return;
    }
    vec4 texel=textureLod(sourceImage,fract(position*kLeaves),probe.lod);
    // Mode 2: the production foliage coverage test of the leaves.
    // Mode 3: two layers of leaves, the second with the layer key of probe.seed (0: the same seeds).
    // Mode 4: the cutoff test of the leaves.
    float opacity=texel.a;
    if(probe.mode==2u)
        opacity=Foliage(texel,LeafSeed(position,0u));
    else if(probe.mode==3u)
        opacity=max(Foliage(texel,LeafSeed(position,0u)),Foliage(texel,LeafSeed(position,floatBitsToUint(probe.seed))));
    if(opacity<probe.cutoff) discard;
    result=vec4(1.0);
}
