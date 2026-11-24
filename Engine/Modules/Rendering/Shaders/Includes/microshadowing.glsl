#ifndef GE_MICROSHADOWING_GLSL
#define GE_MICROSHADOWING_GLSL

// Pablo Zurita's analytical cone approximation for cosine-weighted material AO.
float GE_MicroshadowVisibility(float materialAO, float NdotL)
{
    if (materialAO >= 1.0)
        return 1.0;
    if (materialAO <= 0.0)
        return 0.0;

    float aoSquared = materialAO * materialAO;
    float coneCosine = sqrt(1.0 - aoSquared * aoSquared * materialAO);
    float directionWeight = clamp(NdotL / coneCosine, 0.0, 1.0);
    return pow(directionWeight, 0.75 / materialAO);
}

#endif
