#ifndef GE_FRAME_SAMPLERS_GLSL
#define GE_FRAME_SAMPLERS_GLSL

// The shared samplers of the frame set (set 0). A frame texture is a separate image that one of
// these samplers reads, so the frame textures cost 3 samplers per stage. Vulkan counts samplers per
// stage against maxPerStageDescriptorSamplers, which is 16 on MoltenVK and in WebGPU; the material
// and contributor sets get the rest. The sky backdrop (forward_fog.glsl) stays a combined image
// sampler: the sky helpers take it as a function argument, which a constructed sampler cannot be.
// The bindings stay above 130: the WGSL cook puts the sampler of a combined image sampler at its
// binding + 64, and no set 0 image is above binding 66.
// RenderServices binds them by name on every world pass (RenderServicesWorldPass.cpp).
layout(set = 0, binding = 140) uniform sampler ge_linearClampSampler;          // linear, linear mip, clamp to edge
layout(set = 0, binding = 141) uniform samplerShadow ge_shadowCompareSampler;  // linear PCF, reverse-Z compare, far border
layout(set = 0, binding = 142) uniform sampler ge_pointClampSampler;           // nearest, no mip, clamp to edge

#endif
