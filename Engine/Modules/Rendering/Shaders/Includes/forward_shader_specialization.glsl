#ifndef GE_FORWARD_SHADER_SPECIALIZATION_GLSL
#define GE_FORWARD_SHADER_SPECIALIZATION_GLSL

// Defaults retain runtime feature selection. World passes can remove absent features.
#ifdef GE_WGSL_COOK
const bool GE_FORWARD_LOCAL_LIGHTS = true;
const bool GE_FORWARD_LOCAL_PROBES = true;
const bool GE_FORWARD_HEX_TILING = true;
const int GE_FORWARD_SHADOW_FILTER = -1;
const bool GE_FORWARD_SHADOW_DEBUG = true;
#else
layout(constant_id = 300) const bool GE_FORWARD_LOCAL_LIGHTS = true;
layout(constant_id = 301) const bool GE_FORWARD_LOCAL_PROBES = true;
layout(constant_id = 302) const bool GE_FORWARD_HEX_TILING = true;
layout(constant_id = 303) const int GE_FORWARD_SHADOW_FILTER = -1;
layout(constant_id = 304) const bool GE_FORWARD_SHADOW_DEBUG = true;
#endif

#endif
