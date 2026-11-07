// Planar demand bounds projected node size and surface deviation over its 3D box.
// Bounds nest inside parent boxes, so child demand cannot exceed parent demand.
// The GE_SHARED_PLANAR_DEMAND block also compiles through the C++ test shim.
#ifndef CBT_PLANAR_DEMAND_GLSL
#define CBT_PLANAR_DEMAND_GLSL

// GE_SHARED_PLANAR_DEMAND_BEGIN
// The view the planar demand reads, from the render-origin-relative view-projection.
struct CBTPlanarView
{
    vec4 RowX;         // clip x as a function of (position, 1)
    vec4 RowY;         // clip y
    vec4 RowW;         // clip w: the view depth under a perspective camera
    float FocalPx;     // the vertical focal length in pixels
    float ViewPerClipX; // view x per clip x (1 / P00)
    float ViewPerClipY; // view y per clip y (1 / P11)
    float Kappa;       // the cosine of the half-diagonal field of view
    float MarginTanX;  // the margin frustum's half tangents
    float MarginTanY;
    float Margin;      // the margin frustum's NDC extent (CBT_SPLIT_NDC_MARGIN)
    uint Ortho;        // 1 for an orthographic camera: the demand is the size at the fixed scale
};

// The least value of row . (p, 1) over the box [lo, hi]: a linear function takes it at a corner.
float CBT_BoxRowMin(vec4 row, vec3 lo, vec3 hi)
{
    precise float v = row.w + min(row.x * lo.x, row.x * hi.x) + min(row.y * lo.y, row.y * hi.y) +
                      min(row.z * lo.z, row.z * hi.z);
    return v;
}

// The greatest value of row . (p, 1) over the box [lo, hi].
float CBT_BoxRowMax(vec4 row, vec3 lo, vec3 hi)
{
    precise float v = row.w + max(row.x * lo.x, row.x * hi.x) + max(row.y * lo.y, row.y * hi.y) +
                      max(row.z * lo.z, row.z * hi.z);
    return v;
}

// The distance from `eye` to the box [lo, hi]: zero inside.
float CBT_DistanceToBox(vec3 lo, vec3 hi, vec3 eye)
{
    precise float dx = max(max(lo.x - eye.x, eye.x - hi.x), 0.0f);
    precise float dy = max(max(lo.y - eye.y, eye.y - hi.y), 0.0f);
    precise float dz = max(max(lo.z - eye.z, eye.z - hi.z), 0.0f);
    precise float dist = sqrt(dx * dx + dy * dy + dz * dz);
    return dist;
}

// The distance from `eye` to the farthest corner of the box [lo, hi].
float CBT_FarthestOfBox(vec3 lo, vec3 hi, vec3 eye)
{
    precise float dx = max(abs(lo.x - eye.x), abs(hi.x - eye.x));
    precise float dy = max(abs(lo.y - eye.y), abs(hi.y - eye.y));
    precise float dz = max(abs(lo.z - eye.z), abs(hi.z - eye.z));
    precise float dist = sqrt(dx * dx + dy * dy + dz * dz);
    return dist;
}

// Whether the box lies wholly behind the eye or wholly outside one side of the margin frustum
// (|x| <= margin x w, |y| <= margin x w, w > 0 in clip space). A sub-box of such a box is too.
bool CBT_BoxOutsideMarginFrustum(CBTPlanarView v, vec3 lo, vec3 hi)
{
    if (v.Ortho != 0u)
        return false;
    if (CBT_BoxRowMax(v.RowW, lo, hi) <= 1.0e-5f)
        return true;
    float m = v.Margin;
    vec4 right = vec4(m * v.RowW.x - v.RowX.x, m * v.RowW.y - v.RowX.y, m * v.RowW.z - v.RowX.z,
                      m * v.RowW.w - v.RowX.w);
    vec4 left = vec4(m * v.RowW.x + v.RowX.x, m * v.RowW.y + v.RowX.y, m * v.RowW.z + v.RowX.z,
                     m * v.RowW.w + v.RowX.w);
    vec4 top = vec4(m * v.RowW.x - v.RowY.x, m * v.RowW.y - v.RowY.y, m * v.RowW.z - v.RowY.z,
                    m * v.RowW.w - v.RowY.w);
    vec4 bottom = vec4(m * v.RowW.x + v.RowY.x, m * v.RowW.y + v.RowY.y, m * v.RowW.z + v.RowY.z,
                       m * v.RowW.w + v.RowY.w);
    return CBT_BoxRowMax(right, lo, hi) < 0.0f || CBT_BoxRowMax(left, lo, hi) < 0.0f ||
           CBT_BoxRowMax(top, lo, hi) < 0.0f || CBT_BoxRowMax(bottom, lo, hi) < 0.0f;
}

// The view depth bound D of the box (meters), `dist` its distance from the eye and `outside` its
// CBT_BoxOutsideMarginFrustum answer: max(least corner depth, kappa x dist, 1 m) in the frustum,
// max(dist, 1 m) outside it. It never shrinks on a sub-box, across the two forms included.
float CBT_PlanarBoxDepth(CBTPlanarView v, vec3 lo, vec3 hi, float dist, bool outside)
{
    if (v.Ortho != 0u)
        return 1.0f;
    if (outside)
        return max(dist, 1.0f);
    precise float kappaDist = v.Kappa * dist;
    precise float depth = max(max(CBT_BoxRowMin(v.RowW, lo, hi), kappaDist), 1.0f);
    return depth;
}

// The demand of a node (pixels): `sizeM` its size sqrt(E^2 + r^2), `depth` its CBT_PlanarBoxDepth.
float CBT_PlanarBoxDemand(CBTPlanarView v, vec3 lo, vec3 hi, float depth, bool outside, float sizeM)
{
    if (v.Ortho != 0u)
    {
        precise float scaled = sizeM * v.FocalPx;
        return scaled;
    }
    if (outside)
    {
        precise float isotropic = sizeM * v.FocalPx / depth;
        return isotropic;
    }
    precise float xMax = max(abs(CBT_BoxRowMin(v.RowX, lo, hi)), abs(CBT_BoxRowMax(v.RowX, lo, hi))) * v.ViewPerClipX;
    precise float yMax = max(abs(CBT_BoxRowMin(v.RowY, lo, hi)), abs(CBT_BoxRowMax(v.RowY, lo, hi))) * v.ViewPerClipY;
    precise float tx = min(xMax / depth, v.MarginTanX);
    precise float ty = min(yMax / depth, v.MarginTanY);
    precise float demand = sizeM * v.FocalPx * sqrt(1.0f + tx * tx + ty * ty) / depth;
    return demand;
}

// The node's size: the undisplaced split edge `edgeM` and the drawn range `rangeM` (meters).
float CBT_PlanarNodeSize(float edgeM, float rangeM)
{
    precise float size = sqrt(edgeM * edgeM + rangeM * rangeM);
    return size;
}

// Bilinear relief is bounded by min(rangeM, stepM*(cellsX+cellsZ)) in meters.
// The steepest lattice step and box extents shrink on sub-boxes, so the bound nests.
float CBT_PlanarStepBoundRange(float rangeM, float stepM, float cellsX, float cellsZ)
{
    precise float stepRange = stepM * (cellsX + cellsZ);
    return min(rangeM, stepRange);
}

// A piecewise-linear profile deviates from its chord by at most the largest slope change
// times min(knots*cells/4, (cells*cells+1)/8). No crossed lattice line means zero deviation.
float CBT_PlanarChordSpan(float cells, float knots)
{
    if (!(knots > 0.0f))
        return 0.0f;
    precise float few = knots * cells * 0.25f;
    precise float wide = (cells * cells + 1.0f) * 0.125f;
    return min(few, wide);
}

// Bounds surface-to-facet distance in meters from lattice second and mixed differences.
// The bound covers axis-leg and axis-hypotenuse facets and shrinks on sub-boxes.
float CBT_PlanarCurvatureBound(float secondXM, float secondZM, float mixedM, float cellsX, float cellsZ,
                               float knotsX, float knotsZ)
{
    precise float alongX =
        secondXM * (CBT_PlanarChordSpan(cellsX, knotsX) + CBT_PlanarChordSpan(0.5f * cellsX, knotsX));
    precise float alongZ =
        secondZM * (CBT_PlanarChordSpan(cellsZ, knotsZ) + CBT_PlanarChordSpan(0.5f * cellsZ, knotsZ));
    precise float twist = mixedM * cellsX * cellsZ * 0.25f;
    return alongX + alongZ + twist;
}

// Child residual intervals are measured against the parent plane in normalized height.
// Center offsets and halfCells use lattice cells; slope differences bound the linear spread.
vec2 CBT_PlaneFoldChild(vec4 child, float parentSlopeX, float parentSlopeZ, float offsetX, float offsetZ,
                        float halfCells)
{
    precise float spread = (abs(child.x - parentSlopeX) + abs(child.y - parentSlopeZ)) * halfCells;
    precise float shift = parentSlopeX * offsetX + parentSlopeZ * offsetZ;
    precise float low = child.z - spread - shift;
    precise float high = child.z + child.w + spread - shift;
    return vec2(low, high);
}

// Fold four child planes by their mean slopes and union of residual intervals.
// The resulting width cannot be smaller than a child's width.
vec4 CBT_PlaneFold(vec4 child0, vec4 child1, vec4 child2, vec4 child3, vec2 offset0, vec2 offset1, vec2 offset2,
                   vec2 offset3, float halfCells)
{
    precise float slopeX = 0.25f * (child0.x + child1.x + child2.x + child3.x);
    precise float slopeZ = 0.25f * (child0.y + child1.y + child2.y + child3.y);
    vec2 r0 = CBT_PlaneFoldChild(child0, slopeX, slopeZ, offset0.x, offset0.y, halfCells);
    vec2 r1 = CBT_PlaneFoldChild(child1, slopeX, slopeZ, offset1.x, offset1.y, halfCells);
    vec2 r2 = CBT_PlaneFoldChild(child2, slopeX, slopeZ, offset2.x, offset2.y, halfCells);
    vec2 r3 = CBT_PlaneFoldChild(child3, slopeX, slopeZ, offset3.x, offset3.y, halfCells);
    float low = min(min(r0.x, r1.x), min(r2.x, r3.x));
    float high = max(max(r0.y, r1.y), max(r2.y, r3.y));
    float widest = max(max(child0.w, child1.w), max(child2.w, child3.w));
    precise float width = high - low;
    return vec4(slopeX, slopeZ, low, max(width, widest));
}

// The flat veto: whether r, `rangeM`, projects at most `flatPx` at the box's nearest point, `dist`
// from the eye. Off when flatPx is 0.
bool CBT_PlanarBoxFlat(CBTPlanarView v, float rangeM, float dist, float flatPx)
{
    if (!(flatPx > 0.0f))
        return false;
    precise float reach = v.Ortho != 0u ? 1.0f : max(dist, 1.0f);
    precise float rangePx = rangeM * v.FocalPx / reach;
    return rangePx <= flatPx;
}

// All levels through floor(lambdaNear) bound the drawn range, capped by stored levels.
// Return zero while any vertex can still sample level 0 (lambdaNear < 1).
uint CBT_DrawnRangeChainLevels(float lambdaNear, uint stored)
{
    if (!(lambdaNear >= 1.0f))
        return 0u;
    return min(uint(min(lambdaNear, float(stored))), stored);
}
// GE_SHARED_PLANAR_DEMAND_END

#endif // CBT_PLANAR_DEMAND_GLSL
