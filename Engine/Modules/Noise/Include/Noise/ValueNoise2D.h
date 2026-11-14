#pragma once

#include "Types/Types.h"

#include <cmath>

// ---- 2D value noise ---------------------------------------------------------
//
// A hashed value per integer lattice corner, blended with smoothstep weights.
// The heightfield's base fill (Terrain HeightfieldData::FillWithNoise) samples
// it, and terrain_height_bake.comp BaseNoiseHash / BaseSmoothNoise mirrors it
// bit for bit on the GPU: the hash's expression order and constants are part
// of that contract, so a change here is a change to the mirror too.

namespace GameEngine::Noise
{

/// @brief One lattice corner's value in [0, 1].
inline float32 ValueLatticeHash(int32 x, int32 z, uint32 seed)
{
    uint32 h = static_cast<uint32>(x) * 374761393u +
               static_cast<uint32>(z) * 668265263u +
               seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float32>(h & 0x7FFFFFFF) / static_cast<float32>(0x7FFFFFFF);
}

/// @brief Value noise at (x, z) in [0, 1]: the four surrounding corners'
/// values, blended with smoothstep weights.
inline float32 ValueNoise2D(float32 x, float32 z, uint32 seed)
{
    const int32 ix = static_cast<int32>(std::floor(x));
    const int32 iz = static_cast<int32>(std::floor(z));
    const float32 fx = x - static_cast<float32>(ix);
    const float32 fz = z - static_cast<float32>(iz);

    const float32 sx = fx * fx * (3.0f - 2.0f * fx);
    const float32 sz = fz * fz * (3.0f - 2.0f * fz);

    const float32 n00 = ValueLatticeHash(ix, iz, seed);
    const float32 n10 = ValueLatticeHash(ix + 1, iz, seed);
    const float32 n01 = ValueLatticeHash(ix, iz + 1, seed);
    const float32 n11 = ValueLatticeHash(ix + 1, iz + 1, seed);

    const float32 nx0 = n00 + sx * (n10 - n00);
    const float32 nx1 = n01 + sx * (n11 - n01);
    return nx0 + sz * (nx1 - nx0);
}

} // namespace GameEngine::Noise
