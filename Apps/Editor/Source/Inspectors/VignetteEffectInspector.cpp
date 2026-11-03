#include "Inspectors/VignetteEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Types/ColorUtils.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{
constexpr Components::VignetteEffect kDefaults{};

} // namespace

void RegisterVignetteEffectInspector()
{
    using namespace InspectorDrag;
    using V = Components::VignetteEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<V>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        AddComponentFloatRowWithDrag<V>(ctx.Parent, "Intensity",
            std::clamp(effect->Intensity, 0.0f, 1.0f), w, e, n, undo,
            "Change Vignette Intensity",
            [](V& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            kDefaults.Intensity,
            "Corner darkening amount (0 = off).",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<V>(ctx.Parent, "Smoothness",
            std::clamp(effect->Smoothness, 0.0f, 1.0f), w, e, n, undo,
            "Change Vignette Smoothness",
            [](V& u, float v) { u.Smoothness = std::clamp(v, 0.0f, 1.0f); },
            kDefaults.Smoothness,
            "Falloff sharpness toward the corners.",
            {}, 0.0f, 1.0f);

        AddToggleRow(ctx.Parent, "Rounded", effect->Rounded,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<V>(w, e, n, undo, "Change Vignette Rounded",
                    [v](V& u) { u.Rounded = v; });
            },
            "Aspect-correct the falloff to a circle (off = follows screen aspect).");

        // Color swatch + RGB label; click opens the color picker.
        {
            const InspectorUI::ColorSwatchRow swatchRow = InspectorUI::AddColorSwatchRow(
                ctx.Parent, "Color", "Tint the corners fade toward (click to pick; double-click to reset to black)",
                ColorUtils::RgbFloatsToArgb(effect->Color),
                InspectorUI::FormatColorRgb(effect->Color[0], effect->Color[1], effect->Color[2]));
            UIElement* swatchRaw = swatchRow.Swatch;
            Label* rgbLabelRaw = swatchRow.Value;

            auto updateUI = [swatchRaw, rgbLabelRaw, w, e]() {
                auto* c = w->GetComponent<V>(e);
                if (!c) return;
                InspectorUI::SetColorSwatchColor(swatchRaw, ColorUtils::RgbFloatsToArgb(c->Color));
                rgbLabelRaw->SetText(InspectorUI::FormatColorRgb(c->Color[0], c->Color[1], c->Color[2]));
            };

            auto clickHandler = [w, e, n, undo, openPicker, updateUI](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();
                if (!openPicker)
                    return;
                auto* comp = w->GetComponent<V>(e);
                if (!comp)
                    return;

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();
                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<V>(w, e, n, "Vignette Color");
                    *edit = undo->BeginInteractiveEdit("Change Vignette Color", std::move(target));
                }

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<V>(e);
                            if (c) ColorUtils::ArgbToRgbFloats(newArgb, c->Color);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<V>(e);
                        if (!c) return;
                        V updated = *c;
                        ColorUtils::ArgbToRgbFloats(newArgb, updated.Color);
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onCancel = [edit, updateUI]() {
                    if (*edit) edit->Cancel();
                    updateUI();
                };
                cbs.onValueChanging = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<V>(e);
                            if (c) ColorUtils::ArgbToRgbFloats(newArgb, c->Color);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<V>(e);
                        if (!c) return;
                        V updated = *c;
                        ColorUtils::ArgbToRgbFloats(newArgb, updated.Color);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(ColorUtils::RgbFloatsToArgb(comp->Color), 1.0f, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
        }
    };

    InspectorRegistry::Get().RegisterComponentInspector<V>(std::move(fn));
}

} // namespace GameEngine
