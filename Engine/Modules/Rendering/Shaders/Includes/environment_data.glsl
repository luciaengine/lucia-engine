#ifndef GE_ENVIRONMENT_DATA_GLSL
#define GE_ENVIRONMENT_DATA_GLSL

struct GE_LocalProbeData
{
    vec4 positionIntensity;
    vec4 centerBlend;
    vec4 halfExtentsProjection;
    vec4 axisX;
    vec4 axisY;
    vec4 axisZ;
    vec4 params;
    uvec4 control; // priority, ready, influence shape, reserved
    vec4 projectionCenter;
    vec4 projectionHalfExtents;
};
layout(set = 0, binding = 21) uniform EnvDataUBO {
    float iblIntensity;     // linear radiance scale on env-SOURCED radiance only, pre-exposure.
                            // Folded inside GE_SampleEnvironmentIrradiance/Prefilter — never on
                            // the GE_EvaluateIBL returns (see the sampler comment below).
    float prefilterMaxMip;  // = mipCount - 1, the highest valid specular mip INDEX
    float localBoxProjection;
    float lowerHemisphereDarkness;
    vec4 localProbePositionWS;
    vec4 localBoxCenterWS;
    vec4 localBoxHalfExtentsWS;
    vec4 localBoxAxisXWS;
    vec4 localBoxAxisYWS;
    vec4 localBoxAxisZWS;
    vec4 ambientTintSky;      // scene-linear RGB in .rgb (.a unused); white = identity
    vec4 ambientTintEquator;
    vec4 ambientTintGround;
    // Additive authored ambient irradiance FLOOR (AmbientLight component). Scene-linear RGB in
    // .rgb, ALREADY premultiplied by intensity/203 on the CPU. ambientFloorParams.x is the mode
    // (0 = off, 1 = flat, 2 = trilight); .y is affectSpecular (0/1). Mode 0 (no component) makes
    // GE_AmbientFloor return exactly vec3(0), so the diffuse term is byte-identical.
    vec4 ambientFloorSky;
    vec4 ambientFloorEquator;
    vec4 ambientFloorGround;
    vec4 ambientFloorParams;
    GE_LocalProbeData localProbes[2];
} Env;

#endif
