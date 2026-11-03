// Double-sided vertical cards, centered on a landing point. Clip the card to a
// moving thin droplet. Shared clocks synchronize rainfall with ground impacts.
// @property float waterDropSizeOffset "Water Drop Size Offset" default=0 range=-1.75,1.75
// @property float splashStrength "Splash Strength" default=1 range=0,2
// @property float fallingVisibility "Falling Visibility" default=1 range=0,1
// @property float refractionStrength "Refraction Strength" default=0.025 range=0,0.1
// @property float rainIntensity "Rain Intensity" default=0.7 range=0,1
// @property float rainSpeed "Rain Speed" default=1 range=0.1,3
// @property float fallHeight "Fall Height (m)" default=8 range=0.1,20
// @property float groundHeight "Landing Height (m)" default=0 range=-20,20
// @property float dropWidth "Drop Width (m)" default=0.012 range=0.002,0.04
// @property bool dripMode "Edge Drips" default=false
#define GE_TWO_SIDED_KEEP_NORMAL
#include "Includes/water_weather.glsl"
#include "Includes/rain_impacts.glsl"
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    float dropSize = GE_WaterDropScale(Light.uWeatherWindCount.y, Props.waterDropSizeOffset);
    // Recover the authored card center from its UV mapping before choosing a
    // clock. A ledge card may straddle a cell boundary; all of its pixels must
    // still use the same falling-drop and splash event.
    vec2 dxUv = dFdx(sIn.uv0), dyUv = dFdy(sIn.uv0);
    float uvDet = dxUv.x * dyUv.y - dxUv.y * dyUv.x;
    vec2 cardCenter = sIn.positionWS.xz;
    if (abs(uvDet) > 1e-12)
    {
        vec2 worldPerU = (dFdx(sIn.positionWS.xz) * dyUv.y -
                          dFdy(sIn.positionWS.xz) * dxUv.y) / uvDet;
        cardCenter -= worldPerU * (sIn.uv0.x - 0.5);
    }
    vec2 id = floor(cardCenter / GE_RAIN_CELL_SIZE);
    GE_RainImpact impact = GE_SampleRainImpact(id, Light.uTimeParams.x, Props.rainSpeed);
    vec2 landing = Props.dripMode ? cardCenter : impact.center;
    vec4 weather = GE_WaterWeather(vec3(landing.x, Props.groundHeight, landing.y), Light.uTimeParams.x);
    float age = impact.phase;
    float impactEnabled = GE_RainActive(id, impact.cycle + 1.0, Props.rainIntensity * weather.w);
    float landingEnabled = GE_RainActive(id, impact.cycle, Props.rainIntensity * weather.w);
    float height = Props.fallHeight * (1.0 - (Props.dripMode ? age * age : age));
    float streakLength = (Props.dripMode ? 0.065 : mix(0.13, 0.32, GE_RainHash(id + 6.0))) * sqrt(dropSize);
    // The leading tip reaches the landing height exactly at the impact cycle.
    // Keep the streak above its tip, so it cannot visually hit before the burst.
    float y = sIn.positionWS.y - Props.groundHeight - height - streakLength;
    vec2 cardAxis = abs(sIn.normalWS.z) > abs(sIn.normalWS.x) ? vec2(1, 0) : vec2(0, 1);
    // Rain, ripple and splash use the same world-space anchor. Authored edge
    // drips use their card's center for both falling and splash positions.
    float cardX = Props.dripMode ? (sIn.uv0.x - 0.5) * 0.10 :
                                  dot(sIn.positionWS.xz - impact.center, cardAxis);
    // Cards have finite bounds. A bounded sideways bend follows the wind and
    // returns to the same landing point; splash motion has its own small gust.
    float windAlong = dot(weather.xz, cardAxis);
    float bend = 0.014 * tanh(windAlong * 0.35) * sin(age * 3.14159265);
    float fallingX = cardX - bend;
    float across = abs(fallingX);
    float taper = sqrt(max(0.0, 1.0 - pow(y / streakLength, 2.0)));
    float width = Props.dropWidth * dropSize * taper;
    float aa = max(fwidth(across), 0.0005);
    float fallingOpacity = (1.0 - smoothstep(max(0.0, width - aa), width + aa, across)) *
                           step(abs(y), streakLength) * impactEnabled * Props.fallingVisibility;
    vec2 lens = vec2(clamp(fallingX / max(Props.dropWidth * dropSize, 0.001), -1.0, 1.0),
                     clamp(y / streakLength, -1.0, 1.0) * 0.25);
    float splashOpacity = 0.0;
    vec2 splashLens = vec2(0);
    float aboveGround = sIn.positionWS.y - Props.groundHeight;
    // Reuse the bottom of each crossed rain card for five ballistic particles.
    // They share the completed drop's impact cycle, rather than emitting early.
    // All trajectories stay inside the card's actual width and height bounds.
    if (aboveGround < 0.18 && landingEnabled > 0.0 && Props.splashStrength > 0.0)
    {
        float seconds = impact.secondsSinceImpact;
        for (int i = 0; i < 5; ++i)
        {
            float seed = GE_RainHash(id + vec2(float(i) * 13.7, impact.cycle * 7.3));
            float launch = mix(0.90, 1.55, seed);
            float lifetime = 2.0 * launch / 9.81;
            float life = seconds / lifetime;
            float xVelocity = (float(i) - 2.0) * 0.065;
            vec2 center = vec2(xVelocity * seconds, launch * seconds - 4.905 * seconds * seconds);
            center.x += 0.008 * tanh(windAlong * 0.35) * sin(clamp(life, 0.0, 1.0) * 3.14159265);
            float radius = mix(0.0035, 0.006, seed) * dropSize * min(Props.splashStrength, 1.5) *
                           (1.0 - smoothstep(0.55, 1.0, life));
            vec2 delta = vec2(cardX, aboveGround) - center;
            // Slight vertical stretch reads as a thrown droplet in motion.
            delta.y *= 0.7;
            float coverage = (1.0 - smoothstep(max(0.0, radius - aa), radius + aa, length(delta))) *
                             (1.0 - step(1.0, life)) * min(Props.splashStrength, 1.0);
            if (coverage > splashOpacity)
            {
                splashOpacity = coverage;
                splashLens = clamp(delta / max(radius, 0.001), -1.0, 1.0);
            }
        }
    }
    if (splashOpacity > fallingOpacity)
        lens = splashLens;
    o.opacity = max(fallingOpacity, splashOpacity);
    // Use the existing thin transmission path: one scene-color tap, no ray march.
    // A shallow cylindrical lens keeps displacement within a few pixels.
    float facingZ = (mat3(ge_view) * sIn.viewDirWS).z >= 0.0 ? 1.0 : -1.0;
    o.normalWS = normalize(mat3(ge_invView) * vec3(lens.x * Props.refractionStrength,
                                                 lens.y * Props.refractionStrength, facingZ));
    o.baseColor = vec3(0.015);
    o.specularIor = 1.333;
    o.transmissionWeight = 1.0;
    float splashLight = splashOpacity * (0.12 + 0.30 * max(dot(lens, vec2(-0.45, 0.8)), 0.0));
    o.transmissionColor = vec3(0.98, 0.99, 1.0) * (1.0 + splashLight);
    o.emissive = vec3(0.10, 0.12, 0.13) * abs(lens.x);
    o.roughness = 0.05;
    o.planarReflectionMask = 0.0;
    return o;
}
