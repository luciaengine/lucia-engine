// @property vec2 foliageWindHeight "Foliage Base and Height" default=0,1 group=Wind
// @property vec2 foliageWindMotion "Foliage Sway and Leaf Flutter (m)" default=0,0 group=Wind
// @property float foliageLeafPivots "Use Authored Leaf Pivots" default=0 group=Wind
// @deformationEnvelope 1.414214 scaledBy=foliageWindMotion

#ifndef GE_IMPOSTOR_BAKE
#include "Includes/foliage_sway.glsl"

#if defined(HAS_UV6) && defined(HAS_UV7) && !defined(SKINNED) && !defined(GE_USER_PARTICLE_BUFFER)
#define GE_VERTEX_MODIFIER_FRAME
#include "Includes/foliage_leaf_flutter.glsl"

vec3 ModifyVertexFrame(vec3 position, InstanceData inst, out mat3 rotation)
{
    rotation = mat3(1.0);
    // UV6 stores pivot XY. UV7 stores pivot Z and the farthest vertex's distance to it.
    if (Props.foliageLeafPivots <= 0.0 || aUV7.y <= 0.0)
        return GE_ApplyFoliageSway(position, inst, Props.foliageWindHeight, Props.foliageWindMotion.x);

    GE_FoliageWind wind = GE_SampleFoliageWind(inst);
    vec3 pivot = vec3(aUV6, aUV7.x);
    float strength = GE_FoliageWindHeight(pivot.y, Props.foliageWindHeight) * wind.strength;
    if (strength <= 0.0)
        return position;
    float time = inst.deformationTimeSeconds;
    vec3 sway = GE_FoliageSway(wind, time, strength, Props.foliageWindMotion.x);
    if (Props.foliageWindMotion.y <= 0.0)
        return position + sway;

    float localPhase = dot(pivot, vec3(0.73, 1.13, 0.91));
    float flutter = sin(6.2831853 * (time * (2.0 + wind.frequency) + localPhase + wind.phase));
    vec3 pivotOffset = position - pivot;
    float radius = aUV7.y;
    float distanceSquared = dot(pivotOffset, pivotOffset);
    // A malformed radius must still keep the vertex within its culling envelope.
    if (distanceSquared > radius * radius)
        radius = sqrt(distanceSquared);
    float theta = GE_LeafFlutterAngle(flutter * Props.foliageWindMotion.y * strength, radius);
    vec2 sinCos = vec2(sin(theta), cos(theta));
    vec3 axis = vec3(-wind.direction.y, 0.0, wind.direction.x);
    float axisLength = length(axis);
    axis = axisLength > 0.0001 ? axis / axisLength : vec3(1.0, 0.0, 0.0);
    rotation = mat3(GE_RotateFoliageDirection(vec3(1.0, 0.0, 0.0), axis, sinCos),
                    GE_RotateFoliageDirection(vec3(0.0, 1.0, 0.0), axis, sinCos),
                    GE_RotateFoliageDirection(vec3(0.0, 0.0, 1.0), axis, sinCos));
    return GE_FlutterLeafPosition(position, pivot, axis, sinCos, sway);
}
#endif
#endif

vec3 ModifyVertex(vec3 position, InstanceData inst)
{
#if defined(GE_IMPOSTOR_BAKE)
    return position;
#elif defined(GE_VERTEX_MODIFIER_FRAME)
    mat3 rotation;
    return ModifyVertexFrame(position, inst, rotation);
#else
    return GE_ApplyFoliageSway(position, inst, Props.foliageWindHeight, Props.foliageWindMotion.x);
#endif
}
