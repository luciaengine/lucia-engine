#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 1, std140) uniform PixelationParamsBlock
{
    vec4 bandUV[11]; // xy: guarded viewport scale; zw: snap compensation.
    vec4 bandGrid[11]; // xy: cell size; zw: display-grid origin.
    vec4 displayMode; // y: simple screen-space method.
    mat4 invProjection;
} PixelationParams;

layout(push_constant) uniform DepthPixelationPC
{
    float depthPixelationIntensity;
    float depthPixelationNearEnd;
    float depthPixelationMiddleEnd;
    float depthPixelationNearResolution;
    float depthPixelationMiddleResolution;
    float depthPixelationFarResolution;
    float depthPixelationSkyResolution;
    int depthPixelationBandCount;
    int depthPixelationDistanceMode;
    float depthPixelationBand3End;
    float depthPixelationBand4End;
    float depthPixelationBand5End;
    float depthPixelationBand6End;
    float depthPixelationBand7End;
    float depthPixelationBand8End;
    float depthPixelationBand9End;
    int depthPixelationDebugRanges;
} pc;

layout(set = 0, binding = 2) uniform sampler uBandSampler;

layout(set = 0, binding = 3) uniform texture2D uBand0Color;
layout(set = 0, binding = 4) uniform texture2D uBand0Depth;
layout(set = 0, binding = 5) uniform texture2D uBand1Color;
layout(set = 0, binding = 6) uniform texture2D uBand1Depth;
layout(set = 0, binding = 7) uniform texture2D uBand2Color;
layout(set = 0, binding = 8) uniform texture2D uBand2Depth;
layout(set = 0, binding = 9) uniform texture2D uBand3Color;
layout(set = 0, binding = 10) uniform texture2D uBand3Depth;
layout(set = 0, binding = 11) uniform texture2D uBand4Color;
layout(set = 0, binding = 12) uniform texture2D uBand4Depth;
layout(set = 0, binding = 13) uniform texture2D uBand5Color;
layout(set = 0, binding = 14) uniform texture2D uBand5Depth;
layout(set = 0, binding = 15) uniform texture2D uBand6Color;
layout(set = 0, binding = 16) uniform texture2D uBand6Depth;
layout(set = 0, binding = 17) uniform texture2D uBand7Color;
layout(set = 0, binding = 18) uniform texture2D uBand7Depth;
layout(set = 0, binding = 19) uniform texture2D uBand8Color;
layout(set = 0, binding = 20) uniform texture2D uBand8Depth;
layout(set = 0, binding = 21) uniform texture2D uBand9Color;
layout(set = 0, binding = 22) uniform texture2D uBand9Depth;
layout(set = 0, binding = 23) uniform texture2D uSkyColor;

vec2 CellCenter(int band, vec2 uv)
{
    vec2 stepUV = max(PixelationParams.bandGrid[band].xy, vec2(1e-9));
    vec2 origin = PixelationParams.bandGrid[band].zw;
    // Resolve exact cell-boundary ties consistently despite sub-ULP camera
    // compensation rounding during panning.
    vec2 center = (floor((uv - origin) / stepUV + vec2(1e-4)) + 0.5) * stepUV + origin;
    return clamp(center, vec2(0.0), vec2(0.9999999));
}

vec4 Nearest(texture2D tex, int band, vec2 screenUV)
{
    ivec2 size = textureSize(sampler2D(tex, uBandSampler), 0);
    vec4 transform = PixelationParams.bandUV[band];
    // Preserve exact identity coordinates: subtracting and re-adding 0.5
    // can round a cell center onto the wrong texel at odd output sizes.
    vec2 uv = screenUV * transform.xy + 0.5 * (1.0 - transform.xy) + transform.zw;
    // Coarse centers can land exactly on a finer texel boundary. Resolve
    // that tie toward the positive texel despite sub-ULP transform rounding.
    vec2 texel = uv * vec2(size) + vec2(1e-4);
    return texelFetch(sampler2D(tex, uBandSampler), clamp(ivec2(texel), ivec2(0), size - 1), 0);
}

float DepthAt(int band, vec2 uv)
{
    switch (band)
    {
        case 0: return Nearest(uBand0Depth, 0, uv).r;
        case 1: return Nearest(uBand1Depth, 1, uv).r;
        case 2: return Nearest(uBand2Depth, 2, uv).r;
        case 3: return Nearest(uBand3Depth, 3, uv).r;
        case 4: return Nearest(uBand4Depth, 4, uv).r;
        case 5: return Nearest(uBand5Depth, 5, uv).r;
        case 6: return Nearest(uBand6Depth, 6, uv).r;
        case 7: return Nearest(uBand7Depth, 7, uv).r;
        case 8: return Nearest(uBand8Depth, 8, uv).r;
        case 9: return Nearest(uBand9Depth, 9, uv).r;
    }
    return 0.0;
}

vec4 ColorAt(int band, vec2 uv)
{
    switch (band)
    {
        case 0: return Nearest(uBand0Color, 0, uv);
        case 1: return Nearest(uBand1Color, 1, uv);
        case 2: return Nearest(uBand2Color, 2, uv);
        case 3: return Nearest(uBand3Color, 3, uv);
        case 4: return Nearest(uBand4Color, 4, uv);
        case 5: return Nearest(uBand5Color, 5, uv);
        case 6: return Nearest(uBand6Color, 6, uv);
        case 7: return Nearest(uBand7Color, 7, uv);
        case 8: return Nearest(uBand8Color, 8, uv);
        case 9: return Nearest(uBand9Color, 9, uv);
    }
    return Nearest(uSkyColor, 10, uv);
}

float BandEnd(int band)
{
    switch (band)
    {
        case 0: return pc.depthPixelationNearEnd;
        case 1: return pc.depthPixelationMiddleEnd;
        case 2: return pc.depthPixelationBand3End;
        case 3: return pc.depthPixelationBand4End;
        case 4: return pc.depthPixelationBand5End;
        case 5: return pc.depthPixelationBand6End;
        case 6: return pc.depthPixelationBand7End;
        case 7: return pc.depthPixelationBand8End;
        default: return pc.depthPixelationBand9End;
    }
}

float ScreenSampleDistance(vec2 uv, float depth)
{
    vec4 transform = PixelationParams.bandUV[0];
    vec2 sourceUV = uv * transform.xy + 0.5 * (1.0 - transform.xy) + transform.zw;
    ivec2 size = textureSize(sampler2D(uBand0Depth, uBandSampler), 0);
    ivec2 pixel = clamp(ivec2(sourceUV * vec2(size) + vec2(1e-4)), ivec2(0), size - 1);
    sourceUV = (vec2(pixel) + 0.5) / vec2(size);
    vec2 ndc = vec2(sourceUV.x * 2.0 - 1.0, 1.0 - sourceUV.y * 2.0);
    vec4 p = PixelationParams.invProjection * vec4(ndc, depth, 1.0);
    vec3 viewPosition = p.xyz / (abs(p.w) > 1e-8 ? p.w : 1e-8);
    return pc.depthPixelationDistanceMode == 1 ? length(viewPosition) : abs(viewPosition.z);
}

void main()
{
    vec2 sampleUV = CellCenter(10, vUV);
    int sampleBand = 10;
    int range = 10;
    float resultDepth = 0.0;
    int count = clamp(pc.depthPixelationBandCount, 1, 10);
    if (PixelationParams.displayMode.y > 0.5)
    {
        // All band bindings refer to the same already-rendered image. Mask
        // each snapped sample by its depth interval, merging near over far.
        // Preserve original color where no snapped layer covers a surface.
        resultDepth = DepthAt(0, vUV);
        sampleBand = 0;
        sampleUV = resultDepth > 0.0 ? vUV : CellCenter(10, vUV);
        if (resultDepth <= 0.0) resultDepth = DepthAt(0, sampleUV);
        float start = 0.0;
        for (int i = 0; i < count; ++i)
        {
            float end = i + 1 == count ? 1e30 : max(start, BandEnd(i));
            vec2 center = CellCenter(i, vUV);
            float depth = DepthAt(0, center);
            float distance = depth > 0.0 ? ScreenSampleDistance(center, depth) : 1e30;
            if (depth > 0.0 && distance >= start && distance < end)
            {
                sampleUV = center;
                resultDepth = depth;
                range = i;
                break;
            }
            start = end;
        }
    }
    else
    {
        // A cell has one owner. Test coarse coverage at the cell center, then
        // compare all finer bands at that SAME point to preserve depth ordering.
        // Even when another band supplies the nearest surface, its color and depth
        // fill the entire owning cell. Per-output-pixel comparisons would cut small
        // fragments into large cells; skipping a losing owner would leave holes
        // where the winning finer geometry does not cover the rest of that cell.
        for (int i = 0; i < count; ++i)
        {
            vec2 center = CellCenter(i, vUV);
            float depth = DepthAt(i, center);
            if (!(depth > 0.0)) continue;
            range = i;
            sampleBand = i;
            sampleUV = center;
            resultDepth = depth;
            for (int j = i + 1; j < count; ++j)
            {
                float otherDepth = DepthAt(j, center);
                if (otherDepth > resultDepth)
                {
                    sampleBand = j;
                    resultDepth = otherDepth;
                }
            }
            break;
        }
    }
    vec4 result = ColorAt(sampleBand, sampleUV);
    gl_FragDepth = resultDepth;
    if (pc.depthPixelationDebugRanges != 0)
    {
        const vec3 debugColors[11] = vec3[](
            vec3(0.15, 0.85, 0.25), vec3(1.0, 0.75, 0.1), vec3(0.15, 0.4, 1.0),
            vec3(1.0, 0.2, 0.15), vec3(0.1, 0.9, 0.9), vec3(1.0, 0.4, 0.7),
            vec3(0.5, 0.7, 0.1), vec3(1.0, 0.5, 0.05), vec3(0.45, 0.3, 0.85),
            vec3(0.85, 0.9, 0.9), vec3(0.75, 0.2, 0.95));
        oColor = vec4(debugColors[range], 1.0);
        return;
    }
    oColor = result;
}
