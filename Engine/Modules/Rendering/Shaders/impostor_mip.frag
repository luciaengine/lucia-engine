#version 450

layout(set = 0, binding = 0) uniform sampler2D uSourceMip;
layout(location = 0) out vec4 oFiltered;

void main()
{
    // The source and target views expose adjacent mip levels only.
    // A power-of-two tile ends on an even texel through the last generated level.
    ivec2 origin = ivec2(gl_FragCoord.xy) * 2;
    oFiltered = (texelFetch(uSourceMip, origin, 0)
              + texelFetch(uSourceMip, origin + ivec2(1, 0), 0)
              + texelFetch(uSourceMip, origin + ivec2(0, 1), 0)
              + texelFetch(uSourceMip, origin + ivec2(1, 1), 0)) * 0.25;
}
