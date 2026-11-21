// A card shares one pivot, hinge axis, angle and sway translation.
vec3 GE_RotateFoliageDirection(vec3 direction, vec3 axis, vec2 sinCos)
{
    return direction * sinCos.y + cross(axis, direction) * sinCos.x
         + axis * dot(axis, direction) * (1.0f - sinCos.y);
}

vec3 GE_FlutterLeafPosition(vec3 position, vec3 pivot, vec3 axis, vec2 sinCos, vec3 sway)
{
    return pivot + GE_RotateFoliageDirection(position - pivot, axis, sinCos) + sway;
}

// The farthest vertex must be inside cardRadius. Arc length bounds motion in any direction.
float GE_LeafFlutterAngle(float displacementMeters, float cardRadius)
{
    return clamp(displacementMeters / max(cardRadius, 0.001f), -1.2f, 1.2f);
}
