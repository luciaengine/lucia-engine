#ifndef GE_SPRITE_LIGHTING_GLSL
#define GE_SPRITE_LIGHTING_GLSL

// Recover a sprite's painted UV frame from screen derivatives. This works on
// quads without vertex tangents and follows atlas flips, mirrored transforms,
// rotation, and nonuniform scale. Green points along increasing normal-map V.
vec3 GE_SpriteNormalFromDerivatives(vec3 normalWS, vec3 dpdx, vec3 dpdy,
                                  vec2 duvdx, vec2 duvdy, vec3 tangentNormal)
{
    vec3 n = normalize(normalWS);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) < 1e-12f) return n;
    vec3 t = (dpdx * duvdy.y - dpdy * duvdx.y) * (1.0f / determinant);
    vec3 b = (dpdy * duvdx.x - dpdx * duvdy.x) * (1.0f / determinant);
    // Project out the geometric normal and remove scale, retaining the UV
    // handedness independently for each axis (do not rebuild B with cross).
    t = t - n * dot(n, t);
    b = b - n * dot(n, b);
    if (dot(t, t) < 1e-12f || dot(b, b) < 1e-12f) return n;
    return normalize(normalize(t) * tangentNormal.x +
                     normalize(b) * tangentNormal.y + n * tangentNormal.z);
}
#endif
