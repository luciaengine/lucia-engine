vec3 GE_ApplyFoliageSeason(vec3 baseColor, vec3 normal, vec2 mask, vec4 autumnTint, float snowAmount)
{
    mask = clamp(mask, vec2(0.0), vec2(1.0));
    float autumn = clamp(autumnTint.a, 0.0, 1.0) * mask.r;
    vec3 color = mix(baseColor, baseColor * autumnTint.rgb, autumn);
    float snow = clamp(snowAmount, 0.0, 1.0) * mask.g * smoothstep(0.25, 0.8, normal.y);
    return mix(color, vec3(0.85), snow);
}
