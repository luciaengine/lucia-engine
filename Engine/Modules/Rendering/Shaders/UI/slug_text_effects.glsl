// Gaussian coverage of vector glyphs. Integrate horizontally across curve
// crossings, then integrate scanlines vertically. Unlike sparse point masks,
// a wide kernel cannot jump over a narrow stroke in the horizontal direction.
float textGaussianCdf(float value)
{
    // Smooth normal-CDF approximation, with absolute error below 0.0002.
    return 0.5 + 0.5 * tanh(0.7978845608 * (value + 0.044715 * value * value * value));
}

// Beyond this many sigmas a crossing's CDF contribution is below 4e-5.
const float kTextGaussianReachSigmas = 4.0;

int slugBandOfRow(float sampleY, vec4 transform, int maxBand)
{
    return clamp(int(sampleY * transform.y + transform.w), 0, maxBand);
}

// emBounds is the glyph's undilated em box as (xMin, yMax, xMax, yMin). Rows
// outside it and fragments beyond the kernel reach of it hold no ink, so they
// add weight but no fetches.
float slugGaussianCoverage(texture2D curves, utexture2D bands,
                           sampler curveSampler, sampler bandSampler,
                           vec2 coord, vec4 transform, ivec4 glyphData,
                           vec4 emBounds, vec2 emPerPixel, float sigma)
{
    // The fill's antialiasing is a one-device-pixel box; its variance (1/12)
    // is added so a blurred edge matches a blur of the rasterized glyph.
    sigma = sqrt(sigma * sigma + 1.0 / 12.0);
    vec2 emSigma = max(emPerPixel * sigma, vec2(0.000001));
    float reachX = kTextGaussianReachSigmas * emSigma.x;
    if (coord.x < emBounds.x - reachX || coord.x > emBounds.z + reachX)
        return 0.0;
    float yLow = min(emBounds.y, emBounds.w);
    float yHigh = max(emBounds.y, emBounds.w);
    // Scanline spacing: one device pixel, or sigma/8 below sigma 8 so a
    // horizontal edge between two rows is off by at most 1/16 of a row weight.
    // Every blur therefore sums at least 49 rows.
    float rowStep = min(1.0, sigma * 0.125);
    int halfRows = int(ceil(3.0 * sigma / rowStep));
    float sum = 0.0;
    float weights = 0.0;
    [[dont_unroll]] for (int row = -halfRows; row <= halfRows; ++row)
    {
        float y = float(row) * rowStep;
        float weight = exp(-0.5 * y * y / (sigma * sigma));
        weights += weight;
        vec2 sampleCoord = coord + vec2(0.0, y * emPerPixel.y);
        if (sampleCoord.y < yLow || sampleCoord.y > yHigh)
            continue;
        int band = clamp(int(sampleCoord.y * transform.y + transform.w), 0, glyphData.w & 0x00FF);
        uvec2 data = texelFetch(usampler2D(bands, bandSampler),
                              ivec2(glyphData.x + band, glyphData.y), 0).xy;
        ivec2 bandLoc = slugCalcBandLoc(glyphData.xy, data.y);
        int count = int(min(data.x, uint(kMaxCurvesPerBand)));
        float coverage = 0.0;
        [[dont_unroll]] for (int curve = 0; curve < count; ++curve)
        {
            ivec2 loc = ivec2(texelFetch(usampler2D(bands, bandSampler),
                                       ivec2(bandLoc.x + curve, bandLoc.y), 0).xy);
            vec4 p12 = texelFetch(sampler2D(curves, curveSampler), loc, 0) - vec4(sampleCoord, sampleCoord);
            vec2 p3 = texelFetch(sampler2D(curves, curveSampler), ivec2(loc.x + 1, loc.y), 0).xy - sampleCoord;
            // Curves are sorted by descending max x: once one lies beyond the
            // kernel reach to the left, the rest of the band does too.
            if (max(max(p12.x, p12.z), p3.x) < -reachX) break;
            uint code = slugCalcRootCode(p12.y, p12.w, p3.y);
            if (code != 0u)
            {
                vec2 roots = slugSolveHorizPoly(p12, p3) / emSigma.x;
                if ((code & 1u) != 0u) coverage += textGaussianCdf(roots.x);
                if (code > 1u) coverage -= textGaussianCdf(roots.y);
            }
        }
        sum += weight * clamp(abs(coverage), 0.0, 1.0);
    }
    return sum / weights;
}

// Dilation of a vector glyph by radius device pixels. Each row within the
// radius reports the horizontal distance to its nearest crossing, or its own
// vertical offset when the row sample is inside the glyph; the glyph distance
// is the smallest hypotenuse over the rows and the coverage is a one-pixel
// ramp centered on the radius, so width 0 continues the fill's own edge.
float slugDilationCoverage(texture2D curves, utexture2D bands,
                           sampler curveSampler, sampler bandSampler,
                           vec2 coord, vec4 transform, ivec4 glyphData,
                           vec4 emBounds, vec2 emPerPixel, float radius)
{
    float reachPx = radius + 1.0;
    float reachX = reachPx * emPerPixel.x;
    if (coord.x < emBounds.x - reachX || coord.x > emBounds.z + reachX)
        return 0.0;
    float yLow = min(emBounds.y, emBounds.w);
    float yHigh = max(emBounds.y, emBounds.w);
    const float rowStep = 0.5;
    int halfRows = int(ceil(reachPx / rowStep));
    int maxBand = glyphData.w & 0x00FF;
    float pixelsPerEmX = 1.0 / max(emPerPixel.x, 0.000001);
    float distance = reachPx;
    [[dont_unroll]] for (int row = -halfRows; row <= halfRows; ++row)
    {
        float dyPx = float(row) * rowStep;
        if (abs(dyPx) >= distance) continue;
        float sampleY = coord.y + dyPx * emPerPixel.y;
        if (sampleY < yLow || sampleY > yHigh) continue;
        int band = slugBandOfRow(sampleY, transform, maxBand);
        uvec2 data = texelFetch(usampler2D(bands, bandSampler),
                              ivec2(glyphData.x + band, glyphData.y), 0).xy;
        ivec2 bandLoc = slugCalcBandLoc(glyphData.xy, data.y);
        int count = int(min(data.x, uint(kMaxCurvesPerBand)));
        vec2 sampleCoord = vec2(coord.x, sampleY);
        float nearest = distance;
        int winding = 0;
        [[dont_unroll]] for (int curve = 0; curve < count; ++curve)
        {
            ivec2 loc = ivec2(texelFetch(usampler2D(bands, bandSampler),
                                       ivec2(bandLoc.x + curve, bandLoc.y), 0).xy);
            vec4 p12 = texelFetch(sampler2D(curves, curveSampler), loc, 0) - vec4(sampleCoord, sampleCoord);
            vec2 p3 = texelFetch(sampler2D(curves, curveSampler), ivec2(loc.x + 1, loc.y), 0).xy - sampleCoord;
            if (max(max(p12.x, p12.z), p3.x) < -reachX) break;
            uint code = slugCalcRootCode(p12.y, p12.w, p3.y);
            if (code == 0u) continue;
            vec2 roots = slugSolveHorizPoly(p12, p3) * pixelsPerEmX;
            if ((code & 1u) != 0u)
            {
                winding += roots.x > 0.0 ? 1 : 0;
                nearest = min(nearest, abs(roots.x));
            }
            if (code > 1u)
            {
                winding -= roots.y > 0.0 ? 1 : 0;
                nearest = min(nearest, abs(roots.y));
            }
        }
        float rowDistance = winding != 0 ? abs(dyPx) : sqrt(dyPx * dyPx + nearest * nearest);
        distance = min(distance, rowDistance);
    }
    return clamp(radius + 0.5 - distance, 0.0, 1.0);
}
