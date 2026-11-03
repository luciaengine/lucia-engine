#include "Components/Rendering/Sprite2D.h"
#include "Components/ComponentRegistration.h"
#include "ECS/ECSTemplates.h"
GE_REGISTER_COMPONENT(GameEngine::Components::Sprite2D, Texture, Material, NormalMap, U0,V0,U1,V1,PixelsPerUnit,PivotX,PivotY,NormalStrength,Tint,RenderLayers,Visible,FlipX,FlipY,Lit,LinearFiltering);
namespace {
using namespace GameEngine::Components;
const bool polish = SetReflectedFieldTooltip<Sprite2D>("Material", "Optional sprite material: supplies albedo/normal textures, baseColor and opacity. Arbitrary surface shaders are not evaluated by this sprite pass.") &&
 SetReflectedFieldTooltip<Sprite2D>("Lit", "Use scene ambient, directional and point lighting with the normal atlas. No shadow casting.") &&
 SetReflectedFieldTooltip<Sprite2D>("NormalMap", "Normal atlas matching the albedo layout; overrides the material normalMap.") &&
 SetReflectedFieldTooltip<Sprite2D>("RenderLayers", "Camera visibility bitmask; independent from Render Sorting draw order.") &&
 SetReflectedFieldRange<Sprite2D>("PixelsPerUnit",1,512) && SetReflectedFieldRange<Sprite2D>("NormalStrength",0,4);
}
namespace GameEngine::ECS { GE_INSTANTIATE_ENGINE_COMPONENT(Components::Sprite2D); }
