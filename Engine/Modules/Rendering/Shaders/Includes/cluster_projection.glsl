#ifndef GE_CLUSTER_PROJECTION_GLSL
#define GE_CLUSTER_PROJECTION_GLSL

// Inverse-projection depth coefficients, column-major GLSL indexing:
// [2][2], [3][2], [2][3], [3][3]. Unlike the perspective-only near/far
// formula this also reconstructs orthographic reverse-Z depth.
float GE_ClusterViewZ(float depth, float zDepth, float zConstant,
                     float wDepth, float wConstant)
{
    return (zDepth * depth + zConstant) /
           max(wDepth * depth + wConstant, 1e-30f);
}

// A cluster side plane must have an offset for orthographic views. Building
// every plane through the camera origin makes parallel tiles into wedges.
vec4 GE_ClusterInwardPlane(vec3 a, vec3 b, vec3 c, vec3 inside)
{
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(n, inside - a) < 0.0f) n = n * -1.0f;
    return vec4(n, -dot(n, a));
}

struct GE_ClusterSidePlanes
{
    vec4 Left;
    vec4 Right;
    vec4 Top;
    vec4 Bottom;
};

GE_ClusterSidePlanes GE_ClusterBuildSidePlanes(vec3 farTL, vec3 farTR, vec3 farBL, vec3 farBR,
                                              vec3 nearTL, vec3 nearBR, vec3 inside)
{
    // Anchor offsets at a near corner; distant far corners lose precision.
    GE_ClusterSidePlanes planes;
    planes.Left = GE_ClusterInwardPlane(nearTL, farBL, farTL, inside);
    planes.Right = GE_ClusterInwardPlane(nearBR, farTR, farBR, inside);
    planes.Top = GE_ClusterInwardPlane(nearTL, farTL, farTR, inside);
    planes.Bottom = GE_ClusterInwardPlane(nearBR, farBR, farBL, inside);
    return planes;
}

float GE_ClusterPlaneDistance(vec4 plane, vec3 point)
{
    return dot(vec3(plane.x, plane.y, plane.z), point) + plane.w;
}
#endif
