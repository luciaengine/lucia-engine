#ifndef GE_VOGEL_DISK_GLSL
#define GE_VOGEL_DISK_GLSL

// The rotation is (cosine, sine). Samples fill a unit disk with a golden-angle spiral.
vec2 GE_VogelDisk(int sampleIndex, int totalSamples, vec2 rotation)
{
    const float kGoldenAngle = 2.39996323; // 137.5° in radians
    float radius = sqrt((float(sampleIndex) + 0.5) / float(totalSamples));
    float theta = float(sampleIndex) * kGoldenAngle;
    float cosine = cos(theta);
    float sine = sin(theta);
    return radius * vec2(cosine * rotation.x - sine * rotation.y, cosine * rotation.y + sine * rotation.x);
}

#endif
