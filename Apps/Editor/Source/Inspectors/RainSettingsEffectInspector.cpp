#include "Inspectors/RainSettingsEffectInspector.h"
#include "InspectorRegistry.h"
#include "Components/Rendering/PostProcessEffects/RainSettingsEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include <algorithm>

namespace GameEngine
{
void RegisterRainSettingsEffectInspector()
{
    using Rain = Components::RainSettingsEffect;
    using namespace InspectorDrag;
    InspectorRegistry::Get().RegisterComponentInspector<Rain>([](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid()) return;
        const auto* rain = ctx.World->GetComponent<Rain>(ctx.Entity);
        if (!rain) return;
        auto* w = ctx.World;
        const auto e = ctx.Entity;
        auto* n = ctx.ChangeNotifications;
        auto* undo = ctx.Undo;
        AddComponentFloatRowWithDrag<Rain>(ctx.Parent, "Water Drop Size", rain->WaterDropSize, w, e, n, undo,
            "Change Water Drop Size",
            [](Rain& u, float v) { u.WaterDropSize = std::clamp(v, 0.25f, 2.0f); },
            1.0f, "Shared size multiplier for rain, splashes and water beads. Each material can add its own size offset.",
            {}, 0.25f, 2.0f);
        AddComponentFloatRowWithDrag<Rain>(ctx.Parent, "Falling Rain Size Offset", rain->FallingRainSizeOffset, w, e, n, undo,
            "Change Falling Rain Size Offset",
            [](Rain& u, float v) { u.FallingRainSizeOffset = std::clamp(v, -1.75f, 1.75f); },
            0.0f, "Added to the global size for falling rain streaks. The final multiplier is limited to 0.25–2.",
            {}, -1.75f, 1.75f);
        AddComponentFloatRowWithDrag<Rain>(ctx.Parent, "Water Ripple Speed", rain->WaterRippleSpeed, w, e, n, undo,
            "Change Water Ripple Speed",
            [](Rain& u, float v) { u.WaterRippleSpeed = std::clamp(v, 0.0f, 4.0f); },
            1.0f, "Animation speed of the water normals on puddles, windows and wet objects. Zero freezes these waves.",
            {}, 0.0f, 4.0f);
    });
}
}
