// std140 tail of ForwardLightUBO. Each volume occupies seven vec4 lanes.
vec4 uWeatherWindCount; // x=volume count, y=water drop size, z=ripple speed, w=reserved
vec4 uWeatherWind[224];
