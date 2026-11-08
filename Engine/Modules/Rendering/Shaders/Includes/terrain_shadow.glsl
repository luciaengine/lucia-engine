// Sun-aligned clearance samples store the shadow-volume margin and horizontal occluder distance in meters.
// The bake scans upwind suffix maxima for center/lower sun rays; receivers compare margin over an analytic sun disc.
// Shared shader/CPU math requires includer-provided GE_TerrainShadowReadClearance and GE_TerrainShadowReadGround.

// GE_SHARED_TERRAIN_SHADOW_BEGIN
struct GE_TerrainShadowGrid
{
    // The grid's center and the terrain's corner and extent, in the receivers' frame (x, z).
    float CenterX;
    float CenterZ;
    float TerrainX;
    float TerrainZ;
    float TerrainSizeX;
    float TerrainSizeZ;
    // The sun's horizontal direction (unit length): the u axis. v = (-SunZ, SunX).
    float SunX;
    float SunZ;
    // tan of the sun's elevation, meters of rise per meter along u.
    float TanElevation;
    // Meters between samples, and the first sample's u and v.
    float Texel;
    float UMin;
    float VMin;
    // Samples per line and lines in use.
    float SamplesU;
    float SamplesV;
};

// A sample with no caster (outside the terrain), and the magnitude the map can hold (RG16F; the
// limit bounds both the clearance and the occluder distance, in meters).
const float kGE_TerrainShadowNoCaster = -1.0e30f;
const float kGE_TerrainShadowClearanceLimit = 60000.0f;
// The smallest penumbra radius (meters across the ray), so an occluder at the receiver (a facet's
// own terminator) gives a hard edge without dividing by zero.
const float kGE_TerrainShadowMinPenumbraRadius = 1.0e-4f;
// The penumbra radius bound for a caller whose meshes bound theirs by nothing (the ray-traced mask).
const float kGE_TerrainShadowUncappedPenumbra = 1.0e30f;
const float kGE_TerrainShadowPi = 3.14159265f;
const float kGE_TerrainShadowLn2 = 0.693147181f;
// Tangent/cosine floors describe the same minimum grazing elevation, about 0.9 degrees.
// They cap footprint growth and visible-ground masking at the same view angle.
const float kGE_TerrainShadowMinViewTan = 1.0f / 64.0f;
// The lowest view cosine the pixel footprint widens for, so a grazing pixel covers at most 64 times
// its head-on footprint: the terrain surface's floor on the grazing cosine (cbt_surface.glsl
// kCBTMinGrazingCos).
const float kGE_TerrainShadowMinViewCos = 1.0f / 64.0f;
// How fast the view's and the sun's masking stop selecting the same ground as their azimuths part, per
// radian: the fit of Ginneken, Stavridi and Koenderink (1998) used in the height-direction correlated
// masking-shadowing form (GE_TerrainShadowSeenShare).
const float kGE_TerrainShadowAzimuthCorrelation = 4.41f;

// The grid position, in samples, of the point (x, z).
vec2 GE_TerrainShadowGridCoord(GE_TerrainShadowGrid g, float x, float z)
{
    float dx = x - g.CenterX;
    float dz = z - g.CenterZ;
    float u = dx * g.SunX + dz * g.SunZ;
    float v = dz * g.SunX - dx * g.SunZ;
    return vec2((u - g.UMin) / g.Texel, (v - g.VMin) / g.Texel);
}

// The point (x, z) of the grid position `gridCoord` (in samples).
vec2 GE_TerrainShadowGridPoint(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float u = g.UMin + gridCoord.x * g.Texel;
    float v = g.VMin + gridCoord.y * g.Texel;
    return vec2(g.CenterX + u * g.SunX - v * g.SunZ, g.CenterZ + u * g.SunZ + v * g.SunX);
}

// 1 when (x, z) lies on the terrain (its casters and its ground), else 0.
float GE_TerrainShadowOnTerrain(GE_TerrainShadowGrid g, float x, float z)
{
    float tx = (x - g.TerrainX) / g.TerrainSizeX;
    float tz = (z - g.TerrainZ) / g.TerrainSizeZ;
    return (tx >= 0.0f && tx <= 1.0f && tz >= 0.0f && tz <= 1.0f) ? 1.0f : 0.0f;
}

// Sample k of `line`: x = the height that blocks the sun there (kGE_TerrainShadowNoCaster off the
// terrain or past the line's last sample), y = the ground its clearance is measured from (the
// terrain's height there, clamped to its edge off it).
vec2 GE_TerrainShadowCasterAndGround(GE_TerrainShadowGrid g, float k, float line)
{
    vec2 p = GE_TerrainShadowGridPoint(g, vec2(k, line));
    float ground = GE_TerrainShadowReadGround(p.x, p.y);
    float casts = (k <= g.SamplesU - 1.0f && GE_TerrainShadowOnTerrain(g, p.x, p.y) > 0.5f) ? 1.0f : 0.0f;
    return vec2(casts > 0.5f ? ground : kGE_TerrainShadowNoCaster, ground);
}

// Carry xy stores center-ray top/distance; zw stores lower-disc-ray top/distance.
// Keep each ray maximum; equal tops keep a, which callers supply as the nearer caster.
vec4 GE_TerrainShadowHigherCarry(vec4 a, vec4 b)
{
    bool center = b.x > a.x;
    bool lower = b.z > a.z;
    return vec4(center ? b.x : a.x, center ? b.y : a.y, lower ? b.z : a.z, lower ? b.w : a.w);
}

// A caster's own carry at its sample: its height on both rays, at distance 0.
vec4 GE_TerrainShadowCasterCarry(float caster)
{
    return vec4(caster, 0.0f, caster, 0.0f);
}

// A carry seen `samplesDownwind` samples downwind of the sample it was seen from: each ray's top
// falls by its slope, and its caster recedes, per meter. The operator the line's runs are combined
// with, and one step of the walk.
vec4 GE_TerrainShadowCarryDownwind(GE_TerrainShadowGrid g, vec4 carry, float samplesDownwind, float tanLower)
{
    float meters = samplesDownwind * g.Texel;
    return vec4(carry.x - meters * g.TanElevation, carry.y + meters, carry.z - meters * tanLower, carry.w + meters);
}

// Store center-ray clearance/caster in shadow; when lit, use the lower-disc caster and its nonpositive center margin.
// A point sun makes both choices identical. Bilinear zero crossings can differ by one texel between casters.
// One caster per ray approximates overlapping penumbras and can overestimate light behind multiple occluders.
vec2 GE_TerrainShadowSampleValue(GE_TerrainShadowGrid g, vec4 seen, float ground, float tanLower)
{
    float clearance = seen.x - ground;
    vec2 value = vec2(clearance, seen.y);
    if (!(clearance > 0.0f))
        value = vec2(min(seen.z - seen.w * (g.TanElevation - tanLower) - ground, clearance), seen.w);
    return vec2(clamp(value.x, -kGE_TerrainShadowClearanceLimit, kGE_TerrainShadowClearanceLimit),
                min(value.y, kGE_TerrainShadowClearanceLimit));
}

// The shadow volume's top (height above the terrain's base) at the grid position `gridCoord`, as the
// map's bilinear read weighs its four samples: their clearance plus the ground each was measured
// from; y = the occluder distance there. Off the terrain that ground is the terrain's edge height
// clamped outward, which can kink (a cliff at the edge); interpolating the clearance against the
// receiver's own ground there would misplace the shadow along the whole kink line.
vec2 GE_TerrainShadowTop(GE_TerrainShadowGrid g, vec2 gridCoord)
{
    float i0 = floor(gridCoord.x);
    float j0 = floor(gridCoord.y);
    float fx = gridCoord.x - i0;
    float fy = gridCoord.y - j0;
    vec2 p00 = GE_TerrainShadowGridPoint(g, vec2(i0, j0));
    vec2 p10 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0));
    vec2 p01 = GE_TerrainShadowGridPoint(g, vec2(i0, j0 + 1.0f));
    vec2 p11 = GE_TerrainShadowGridPoint(g, vec2(i0 + 1.0f, j0 + 1.0f));
    float g00 = GE_TerrainShadowReadGround(p00.x, p00.y);
    float g10 = GE_TerrainShadowReadGround(p10.x, p10.y);
    float g01 = GE_TerrainShadowReadGround(p01.x, p01.y);
    float g11 = GE_TerrainShadowReadGround(p11.x, p11.y);
    float ga = g00 + (g10 - g00) * fx;
    float gb = g01 + (g11 - g01) * fx;
    vec2 read = GE_TerrainShadowReadClearance(gridCoord);
    return vec2(read.x + ga + (gb - ga) * fy, read.y);
}

// Return receiver margin above its shadow-volume top and horizontal occluder distance.
// Terrain receivers follow the ground field; other receivers retain height unless sunk below terrain.
// Off-terrain receivers compare against the extended volume.
vec2 GE_TerrainShadowMargin(GE_TerrainShadowGrid g, float x, float y, float z, float onGround)
{
    vec2 c = GE_TerrainShadowGridCoord(g, x, z);
    // Beside the grid or upwind of its last sample: no terrain between the receiver and the sun.
    if (c.y < 0.0f || c.y > g.SamplesV - 1.0f || c.x > g.SamplesU - 1.0f)
        return vec2(-kGE_TerrainShadowClearanceLimit, 0.0f);
    if (c.x < 0.0f)
    {
        // Downwind of the grid (off the terrain) nothing casts: the shadow volume's top falls by
        // tan(elevation) per meter from the grid's first sample on the receiver's line, and the
        // occluder recedes by the same meters.
        vec2 edge = GE_TerrainShadowTop(g, vec2(0.0f, c.y));
        float top = edge.x + c.x * g.Texel * g.TanElevation;
        return vec2(top - y, edge.y - c.x * g.Texel);
    }
    if (onGround > 0.5f)
        return GE_TerrainShadowReadClearance(c);
    if (GE_TerrainShadowOnTerrain(g, x, z) < 0.5f)
    {
        vec2 top = GE_TerrainShadowTop(g, c);
        return vec2(top.x - y, top.y);
    }
    float above = max(y - GE_TerrainShadowReadGround(x, z), 0.0f);
    vec2 read = GE_TerrainShadowReadClearance(c);
    return vec2(read.x - above, read.y);
}

// Return sun-disc visibility: 1 lit, 0 fully shadowed. tanHalfAngle=0 gives a hard point-sun compare.
// maxPenumbraRadius caps the transverse radius in meters; onGround follows GE_TerrainShadowMargin.
float GE_TerrainShadowLit(GE_TerrainShadowGrid g, float x, float y, float z, float onGround, float tanHalfAngle,
                          float maxPenumbraRadius)
{
    vec2 margin = GE_TerrainShadowMargin(g, x, y, z, onGround);
    if (tanHalfAngle <= 0.0f)
        return margin.x > 0.0f ? 0.0f : 1.0f;
    float cosElevation = 1.0f / sqrt(1.0f + g.TanElevation * g.TanElevation);
    float radius = max(min(margin.y / cosElevation * tanHalfAngle, maxPenumbraRadius),
                       kGE_TerrainShadowMinPenumbraRadius);
    float t = margin.x * cosElevation / radius;
    if (t >= 1.0f)
        return 0.0f;
    if (t <= -1.0f)
        return 1.0f;
    // The share of a unit disc beyond a chord at signed distance t from its center.
    return (acos(t) - t * sqrt(1.0f - t * t)) / kGE_TerrainShadowPi;
}

// The lit field averages visibility and shadowed-sample log2 occluder distance in map-texel units.
// Sample at the pixel footprint to separate local relief shadow from distant cast shadow.

// Estimate the shadowed share cast within footprintSamples from visibility and mean occluder octaves.
// Assume distances spread uniformly to a longest occluder; approximate the cumulative share with an octave ramp.
// The ramp is 0 below mean-1/ln(2), 0.5 at the mean and 1 at the longest distance.
float GE_TerrainShadowLocalShare(float litShare, float meanOctaves, float footprintSamples)
{
    float shadow = clamp(1.0f - litShare, 0.0f, 1.0f);
    if (shadow <= 0.0f)
        return 0.0f;
    float mean = meanOctaves / shadow;
    float footprint = log2(max(footprintSamples, 1.0f));
    return shadow * clamp((footprint - mean) * (0.5f * kGE_TerrainShadowLn2) + 0.5f, 0.0f, 1.0f);
}

// Blend unresolved-relief masking across draw levels 1..2.
// Taking light away only after fine relief filters out prevents noisy near-chain visibility from speckling the band.
float GE_TerrainShadowUndrawnRelief(float drawLevel)
{
    return clamp(drawLevel - 1.0f, 0.0f, 1.0f);
}

// Estimate visible lit ground from local shadow, view/sun elevations and their horizontal angle.
// Correlated groove masking uses joint visibility 1/(1+max(L)+c*min(L)); sunward views hide lit faces first.
// Distant cast shadow stays proportional; without local relief, preserve the field share.
float GE_TerrainShadowSeenShare(float litShare, float localShadow, float tanView, float tanSun, float sunSide)
{
    float local = clamp(localShadow, 0.0f, 1.0f);
    float open = 1.0f - local;
    if (local <= 0.0f || open <= 0.0f)
        return litShare;
    float relief = local * max(tanSun, 0.0f);
    float clear = open * max(tanView, kGE_TerrainShadowMinViewTan);
    float visible = clear / (relief + clear);
    float sunHidden = local / open;
    float viewHidden = relief / clear;
    float azimuth = min(acos(clamp(sunSide, -1.0f, 1.0f)), 0.5f * kGE_TerrainShadowPi);
    float correlation = kGE_TerrainShadowAzimuthCorrelation * azimuth /
                        (kGE_TerrainShadowAzimuthCorrelation * azimuth + 1.0f);
    // The ground outside the local shadow that the pixel sees, as a share of all the ground.
    float seenOpen = 1.0f / (1.0f + max(sunHidden, viewHidden) + correlation * min(sunHidden, viewHidden));
    if (sunSide < 0.0f)
        seenOpen = mix(seenOpen, max(open + visible - 1.0f, 0.0f), min(-sunSide, 1.0f));
    return clamp(litShare * seenOpen / (open * visible), 0.0f, 1.0f);
}

// Widen headOn footprint by grazing angle: no widening at draw level 0, full 1/cosine at level 1.
// Intermediate levels use (1/cosine)^drawLevel. Both directional-shadow modes share this rule.
float GE_TerrainShadowFootprint(float headOn, float viewCos, float drawLevel)
{
    float spread = 1.0f / max(viewCos, kGE_TerrainShadowMinViewCos);
    return headOn * pow(spread, clamp(drawLevel, 0.0f, 1.0f));
}

// Convert sampled field visibility/occluder octaves into the share visible from this pixel.
// Always show additional visible light; reduce light only by the unresolved-relief share at drawLevel.
float GE_TerrainShadowFieldShown(GE_TerrainShadowGrid g, vec2 read, float samplesPerPixel, vec2 toEyeAcross,
                                 float viewCos, float drawLevel)
{
    float local = GE_TerrainShadowLocalShare(read.x, read.y, samplesPerPixel);
    float across = length(toEyeAcross);
    float sunSide = across > 0.0f ? dot(toEyeAcross / across, vec2(g.SunX, g.SunZ)) : 0.0f;
    float cosView = clamp(viewCos, 0.0f, 1.0f);
    // The view's elevation above the mean surface as a tangent, at most 64 (a view straight down).
    float tanView = cosView / max(sqrt(1.0f - cosView * cosView), cosView * kGE_TerrainShadowMinViewTan);
    float seen = GE_TerrainShadowSeenShare(read.x, local, tanView, g.TanElevation, sunSide);
    return seen >= read.x ? seen : mix(read.x, seen, GE_TerrainShadowUndrawnRelief(drawLevel));
}
// GE_SHARED_TERRAIN_SHADOW_END

#ifdef GE_TERRAIN_SHADOW_READS_FIELD
// Both shadow modes use this terrain-shadow entry point. Field readers define
// GE_TerrainShadowReadField(gridCoord,level) for its two channels and GE_TerrainShadowFieldLevels() for availability.

// Compute terrain visibility at a receiver height above the terrain base using the shared pixel footprint.
// Blend the lit field in over one to two map samples per pixel; at two samples it replaces the map read.
// Footprint inputs describe view direction, mean-surface cosine and continuous draw level.
float GE_TerrainShadowFootprintLit(GE_TerrainShadowGrid g, float x, float y, float z, float onGround,
                                   float tanHalfAngle, float maxPenumbraRadius, float headOn, vec3 toEye,
                                   float viewCos, float drawLevel)
{
    float samplesPerPixel = GE_TerrainShadowFootprint(headOn, viewCos, drawLevel) / g.Texel;
    float levels = GE_TerrainShadowFieldLevels();
    float field = 1.0;
    float fieldWeight = 0.0;
    if (levels > 0.0 && samplesPerPixel > 1.0)
    {
        vec2 c = GE_TerrainShadowGridCoord(g, x, z);
        if (c.x >= 0.0 && c.y >= 0.0 && c.x <= g.SamplesU - 1.0 && c.y <= g.SamplesV - 1.0)
        {
            // Level 0 holds 2 x 2 samples.
            float level = clamp(log2(samplesPerPixel) - 1.0, 0.0, levels - 1.0);
            field = GE_TerrainShadowFieldShown(g, GE_TerrainShadowReadField(c, level), samplesPerPixel, toEye.xz,
                                               viewCos, drawLevel);
            fieldWeight = smoothstep(1.0, 2.0, samplesPerPixel);
        }
    }
    if (fieldWeight >= 1.0)
        return field;
    float lit = GE_TerrainShadowLit(g, x, y, z, onGround, tanHalfAngle, maxPenumbraRadius);
    return fieldWeight > 0.0 ? mix(lit, field, fieldWeight) : lit;
}
#endif
