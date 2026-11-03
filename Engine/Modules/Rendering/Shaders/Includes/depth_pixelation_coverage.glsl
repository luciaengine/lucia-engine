#ifndef GE_DEPTH_PIXELATION_COVERAGE_GLSL
#define GE_DEPTH_PIXELATION_COVERAGE_GLSL

// Test the geometry's pixel footprint against the distance interval, rather
// than clipping its triangle at an infinitesimally thin band plane. Adjacent
// grids may sample on opposite sides of that plane. A full-pixel derivative
// guard covers both the raster half-footprint and the display-cell center
// offset introduced by quantization and projection-snap compensation. This
// keeps adjacent display cells from exposing a clear-color slit. Rasterization still determines the object's silhouette:
// this never dilates coverage beyond the primitive into the sky/background.
bool GE_DepthPixelationKeep(vec3 viewPosition, vec4 band)
{
    if (band.z < 0.5) return true;
    if (band.y <= band.x) return false;
    float distance = band.w > 0.5 ? length(viewPosition) : abs(viewPosition.z);
    float footprint = fwidth(distance);
    return distance + footprint >= band.x && distance - footprint <= band.y;
}
#endif
