#ifndef GE_FOREST_SPLAT_DISK_GLSL
#define GE_FOREST_SPLAT_DISK_GLSL

// The disk of one splat cell on descending views. A leaf disk turns toward the view axis and grows,
// so the crown seen from above keeps the coverage of its leaves; bark and the rooted base keep their
// baked disk. The baked normal still lights the disk.
struct GE_ForestSplatDisk
{
    vec3 normal; // The disk plane normal, in the tree's local frame.
    float growth; // The disk radius scale.
};

const float kForestSplatOverheadStart = 0.53; // |view axis Y| where the turn and the growth start.
const float kForestSplatOverheadFull = 0.78;  // |view axis Y| where they are complete.
const float kForestSplatOverheadGrowth = 1.15; // The radius scale of a leaf disk seen from straight above.

// `localNormal` and `localViewAxis` are in the tree's local frame (yaw removed).
GE_ForestSplatDisk GE_ForestSplatDiskFor(vec3 localNormal, vec3 localViewAxis, float bend, float leafFraction)
{
    GE_ForestSplatDisk disk = GE_ForestSplatDisk(localNormal, 1.0);
    const float overhead = smoothstep(kForestSplatOverheadStart, kForestSplatOverheadFull, abs(localViewAxis.y));
    if (bend <= 0.0 || leafFraction < 0.5 || overhead <= 0.0)
        return disk;
    const vec3 facing = dot(localNormal, localViewAxis) < 0.0 ? -localViewAxis : localViewAxis;
    disk.normal = normalize(mix(localNormal, facing, overhead));
    disk.growth = mix(1.0, kForestSplatOverheadGrowth, overhead);
    return disk;
}

#endif
