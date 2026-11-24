// A round splat disk of a packed forest stand, with the baked cell color in custom0.rgb.
// The vertex stage turns a leaf normal to the side the camera sees; the back face keeps it.
#define GE_TWO_SIDED_KEEP_NORMAL
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.opacity = float(dot(sIn.uv0, sIn.uv0) <= 1.0);
#if !defined(GE_DEPTH_ONLY_FRAGMENT) && !defined(GE_MOTION_VECTORS)
    o.baseColor = sIn.custom0.rgb;
    o.normalWS = normalize(sIn.normalWS);
    o.roughness = 0.82;
    o.metallic = 0.0;
#endif
    return o;
}
