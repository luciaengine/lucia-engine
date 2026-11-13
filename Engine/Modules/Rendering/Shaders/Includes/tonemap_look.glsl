// The look values that select an arm of tonemap.frag (Components::Look and Passes::kTonemapNoLookArm).
// Neutral (0) is the default arm.
#ifndef GE_TONEMAP_LOOK_GLSL
#define GE_TONEMAP_LOOK_GLSL

const int kLookFilmic = 1;
const int kLookPhotographic = 2;
// The no-look debug arm: identity after exposure. A view selects it; a volume never does.
const int kNoLookArm = 3;

#endif // GE_TONEMAP_LOOK_GLSL
