#ifndef GE_SSSR_HISTORY_POLICY_GLSL
#define GE_SSSR_HISTORY_POLICY_GLSL

// Scalar policy shared by the shader and CPU regression tests. Positive tests
// also reject NaN inputs, without treating a missing ray as a traced miss.

// Bound on the rounding error of m2 - m1 * m1 from RGBA16F moments, as a fraction of m2:
// m2 rounds by at most 2^-11 of itself, m1 * m1 by at most twice that, and m2 >= m1 * m1.
const float kSssrMomentRoundoff = 3.0f / 2048.0f;

// The temporal luminance variance of stored moments (x = mean, y = mean square). A variance
// inside the storage rounding error is zero, so a converged pixel reads a steady variance.
float GE_SssrTemporalVariance(vec2 moments)
{
    return max(moments.y - moments.x * moments.x - kSssrMomentRoundoff * moments.y, 0.0f);
}

// A lobe this smooth traces one fixed ray: no VNDF sample and no frame term.
bool GE_SssrIsMirrorLobe(float roughness)
{
    return roughness < 0.06f;
}

// The frame the march start offset reads its blue noise at. A mirror lobe gets
// spatial noise only, so a still view gives the same hits every frame.
uint GE_SssrStartOffsetFrame(float roughness, uint frameIndex)
{
    return GE_SssrIsMirrorLobe(roughness) ? 0u : frameIndex + 17u;
}

bool GE_SssrDepthHistoryValid(float storedZ, float expectedZ)
{
    return storedZ > 0.0f && expectedZ > 0.0f &&
           abs(storedZ - expectedZ) <= max(0.0001f, expectedZ * 0.02f);
}

bool GE_SssrHitHistoryValid(float roughness, float confidence, float hitDistance,
                          float oldDistance)
{
    if (roughness >= 0.35f || confidence <= 0.0f || oldDistance < 0.0f)
        return true;
    bool sameKind = (hitDistance > 0.0f) == (oldDistance > 0.0f);
    float tolerance = max(0.1f, max(hitDistance, oldDistance) * mix(0.1f, 0.5f, roughness / 0.35f));
    return sameKind && abs(hitDistance - oldDistance) <= tolerance;
}

float GE_SssrHistoryWeight(bool validHistory, bool currentValid, float historyLength,
                          float roughness, float disagreement, float clipRadius)
{
    if (!validHistory || !(historyLength > 0.0f)) return 0.0f;
    if (!currentValid) return 1.0f;
    float sampleCount = historyLength * 32.0f;
    float matureWeight = mix(0.86f, 0.97f, roughness);
    float weight = min(matureWeight, sampleCount / (sampleCount + 1.0f));
    float change = smoothstep(clipRadius, clipRadius * 3.0f, disagreement);
    return weight * (1.0f - 0.8f * change);
}
// Log depth keeps relative precision in RGBA16F without the far-distance
// collapse of a fixed raw-depth tolerance. -65504 is the sky sentinel.
float GE_SssrDecodeHistoryDepth(float logDepth)
{
    return logDepth > -64.0f ? exp2(logDepth) : 0.0f;
}

// Rate is the side of the sampling lattice: 1, 2 or 4. Near mirrors stay
// full-rate. Newly exposed pixels get an immediate sample; unstable rough
// pixels spend more rays, while well-converged rough lobes can reuse history.
uint GE_SssrAdaptiveRate(uint baseRate, float roughness, bool validHistory,
                         float relativeVariance, float historyLength)
{
    if (roughness < 0.35f || !validHistory) return 1u;
    if (relativeVariance > 0.1f || historyLength < 0.125f)
        return max(1u, baseRate / 2u);
    if (relativeVariance < 0.01f && historyLength > 0.5f)
        return min(4u, baseRate * 2u);
    return baseRate;
}

float GE_SssrTraceDistance(float originZ, float directionZ, float nearZ, float maxDistance)
{
    if (directionZ >= 0.0f) return maxDistance;
    // End just inside the near plane, where perspective projection is finite.
    float nearDistance = (originZ - nearZ * 1.01f) / -directionZ;
    return max(0.0f, min(maxDistance, nearDistance));
}
#endif
