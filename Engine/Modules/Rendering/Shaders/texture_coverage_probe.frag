#version 450
#include "Includes/mask_coverage.glsl"
#include "Includes/integer_hash.glsl"
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Probe { float lod; float cutoff; vec2 phase; uint mode; float seed; } probe;
// Modes 2 to 5 draw a grid of kLeaves x kLeaves leaves. Each leaf is one copy of the image with its
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
// Modes 6 and 7 draw the same grid as masked cards, with the seed of a masked surface: each card is a
// quad of four vertices in fan order, so its triangles start at the card's first vertex.
float CardSeed(vec2 position,uint instanceIndex)
{
    uvec2 cell=uvec2(ivec2(floor(position*kLeaves)))&uvec2(15u);
    uint firstVertex=(cell.y*16u+cell.x)*4u;
    return GE_MaskCoverageThreshold(GE_MaskCoverageSeed(firstVertex,instanceIndex));
}
// The masked surface's far test of the card at `position`, with tint alpha `alphaScale`.
float MaskedCard(vec2 position,float alphaScale)
{
    float footprint=exp2(probe.lod)/float(textureSize(sourceImage,0).x);
    float shapeFootprint=min(footprint,kFoliageShapeFootprint);
    float alpha=textureGrad(sourceImage,fract(position*kLeaves),vec2(shapeFootprint,0.0),vec2(0.0,shapeFootprint)).a;
    return GE_MaskCoverageOpacity(alpha,alphaScale,probe.cutoff,GE_MaskCoverageTailWeight(footprint),CardSeed(position,0u));
}
// A grazing card: the sample's footprint is 2^probe.lod texels along x and one texel along y, as a
// card seen edge-on. Mode 9 selects its test from the production footprint, as standard_pbr does;
// mode 10 is the cutoff test of the same sample.
float GrazingCard(vec2 position,bool production)
{
    vec2 size=vec2(textureSize(sourceImage,0));
    vec2 uv=fract(position*kLeaves);
    vec2 uvDx=vec2(exp2(probe.lod)/size.x,0.0);
    vec2 uvDy=vec2(0.0,1.0/size.y);
    float alpha=textureGrad(sourceImage,uv,uvDx,uvDy).a;
    if(!production)
        return step(probe.cutoff,alpha);
    float footprint=GE_AnisotropicMaskFootprint(uvDx,uvDy,0.0);
    float tail=GE_MaskCoverageTailWeight(footprint);
    if(tail<=0.0)
        return step(probe.cutoff,alpha);
    float shape=GE_MaskCoverageShapeGradient(0.0);
    alpha=textureGrad(sourceImage,uv,vec2(shape,0.0),vec2(0.0,shape)).a;
    return GE_MaskCoverageOpacity(alpha,1.0,probe.cutoff,tail,CardSeed(position,0u));
}
// The bounded coverage sample at `position`, read at probe.lod as a sampler would.
float Foliage(vec2 position,float layerSeed)
{
    float footprint=exp2(probe.lod)/float(textureSize(sourceImage,0).x);
    float shapeFootprint=min(footprint,kFoliageShapeFootprint);
    float alpha=textureGrad(sourceImage,fract(position*kLeaves),vec2(shapeFootprint,0.0),vec2(0.0,shapeFootprint)).a;
    float tailWeight=GE_MaskCoverageTailWeight(footprint);
    if(probe.mode==5u)
        return GE_MaskCoverageLeafOpacity(alpha,1.0,probe.cutoff,tailWeight,layerSeed);
    return GE_MaskCoverageTestOpacity(alpha,probe.cutoff,tailWeight,layerSeed);
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
    // Mode 2: the generic foliage coverage test of the leaves.
    // Mode 3: two layers of leaves, the second with the layer key of probe.seed (0: the same seeds).
    // Mode 4: the cutoff test of the leaves.
    // Mode 5: the EZ Tree foliage coverage test.
    // Mode 6: the coverage test of masked cards of instance 0.
    // Mode 7: two instances of masked cards, 0 and the instance of probe.seed's bits (0: the same instance).
    // Mode 8: the masked cards of mode 6 with the tint alpha probe.seed.
    // Modes 9 and 10: grazing cards with the production test and with the cutoff test.
    float opacity=textureLod(sourceImage,fract(position*kLeaves),probe.lod).a;
    if(probe.mode==2u || probe.mode==5u)
        opacity=Foliage(position,LeafSeed(position,0u));
    else if(probe.mode==3u)
        opacity=max(Foliage(position,LeafSeed(position,0u)),Foliage(position,LeafSeed(position,floatBitsToUint(probe.seed))));
    else if(probe.mode==6u)
        opacity=Foliage(position,CardSeed(position,0u));
    else if(probe.mode==7u)
        opacity=max(Foliage(position,CardSeed(position,0u)),Foliage(position,CardSeed(position,floatBitsToUint(probe.seed))));
    else if(probe.mode==8u)
        opacity=MaskedCard(position,probe.seed);
    else if(probe.mode==9u||probe.mode==10u)
        opacity=GrazingCard(position,probe.mode==9u);
    if(opacity<probe.cutoff) discard;
    result=vec4(1.0);
}
