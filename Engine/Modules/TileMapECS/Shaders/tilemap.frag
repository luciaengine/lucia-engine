#version 450

layout(set = 0, binding = 2) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vTint;

layout(location = 0) out vec4 outColor;

void main()
{
    // No discard: the pass attaches no depth and writes none, so a transparent
    // texel already contributes nothing under SrcAlpha blending — and pixel art
    // makes transparent texels the common case, which is exactly the workload a
    // discard would cost early-Z for.
    outColor = texture(uAtlas, vUV) * vTint;
}
