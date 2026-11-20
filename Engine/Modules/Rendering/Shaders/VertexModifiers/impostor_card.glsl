// Impostor card vertex modifier: places the four corners of a quad (UV0 in [0,1]) on the plane
// that faces the instance's viewer, centred on the impostor sphere and as wide as it.
// Object space; the adapter applies the instance transform.
//
// ModifyInstance writes custom0 for Surfaces/impostor.glsl: xy the octahedral code of the
// direction from the sphere center toward the viewer in the object's frame, zw the cosine and
// sine of the object's yaw. The viewer comes from the pass's rebased view-projection: the eye of
// a perspective pass, and the direction toward the light of an orthographic (cascade) pass, so a
// cascade draws the card that the light sees.
//
// @property vec4 impostorBounds "Impostor Bounds" default=0,0,0,1 group=Impostor

#include "Includes/impostor_frame.glsl"

#define GE_VERTEX_MODIFIER_INSTANCE 1

// The viewer of the pass relative to the render origin: xyz a point (w = 1) or a direction
// toward the viewer (w = 0).
vec4 GE_ImpostorViewer()
{
    if (Cam.uCameraPos.w < 0.0)
    {
        // Dual-paraboloid face: the eye is the origin of the rebased view.
        mat3 rotation = mat3(Cam.uVRel);
        return vec4(-(transpose(rotation) * Cam.uVRel[3].xyz), 1.0);
    }
    mat4 m = Cam.uVPRel;
    vec4 row0 = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    vec4 row1 = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    vec4 row2 = vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
    vec4 row3 = vec4(m[0][3], m[1][3], m[2][3], m[3][3]);
    if (dot(abs(row3.xyz), vec3(1.0)) < 1e-9)
    {
        // Orthographic. Reverse-Z: depth grows toward the viewer.
        return vec4(normalize(row2.xyz), 0.0);
    }
    // Perspective: the eye is the point with clip x = y = w = 0.
    mat3 rows = transpose(mat3(row0.xyz, row1.xyz, row3.xyz));
    return vec4(inverse(rows) * -vec3(row0.w, row1.w, row3.w), 1.0);
}

void ModifyInstance(inout InstanceData inst)
{
    vec3 axisX = normalize(inst.modelMatrix[0].xyz);
    vec3 axisY = normalize(inst.modelMatrix[1].xyz);
    vec3 axisZ = normalize(inst.modelMatrix[2].xyz);
    vec4 viewer = GE_ImpostorViewer();
    vec3 toViewer = viewer.xyz;
    if (viewer.w > 0.5)
    {
        vec3 centerRel = vec3(inst.sector - Cam.uRenderOriginSector.xyz) * GE_SECTOR_SIZE
                         + (inst.modelMatrix * vec4(Props.impostorBounds.xyz, 1.0)).xyz;
        toViewer = viewer.xyz - centerRel;
    }
    vec3 local = vec3(dot(toViewer, axisX), dot(toViewer, axisY), dot(toViewer, axisZ));
    float localLength = length(local);
    local = localLength > 1e-6 ? local / localLength : vec3(0.0, 1.0, 0.0);
    vec2 code = Props.impostorAtlas.y < 0.5 ? GE_HemiOctEncode(local) : GE_OctEncode(local);
    float yawLength = max(length(vec2(axisX.x, axisX.z)), 1e-6);
    inst.custom0 = vec4(code, axisX.x / yawLength, -axisX.z / yawLength);
}

vec3 ModifyVertex(vec3 position, InstanceData inst)
{
    vec3 right;
    vec3 up;
    GE_ImpostorFrameAxes(GE_ImpostorDecode(inst.custom0.xy, Props.impostorAtlas), right, up);
#ifdef HAS_UV0
    // UV (0,0) is the card's top-left, as in a baked tile.
    vec2 corner = vec2(aUV0.x * 2.0 - 1.0, 1.0 - aUV0.y * 2.0);
#else
    vec2 corner = vec2(0.0);
#endif
    return Props.impostorBounds.xyz + (right * corner.x + up * corner.y) * Props.impostorBounds.w;
}
