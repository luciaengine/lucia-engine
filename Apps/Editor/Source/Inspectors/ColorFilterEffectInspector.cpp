#include "Inspectors/ColorFilterEffectInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Types/ColorUtils.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{
using Components::ColorFilterBlendMode;

static constexpr EnumEntry<ColorFilterBlendMode> kBlendModes[] = {
    {ColorFilterBlendMode::Multiply, "Multiply"},
    {ColorFilterBlendMode::Add,      "Add"},
    {ColorFilterBlendMode::Screen,   "Screen"},
    {ColorFilterBlendMode::SoftLight,"Soft Light"},
};

} // namespace

void RegisterColorFilterEffectInspector()
{
    using namespace InspectorDrag;
    using CF = Components::ColorFilterEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<CF>(ctx.Entity);
        if (!effect)
        {
            InspectorUI::AddLine(ctx.Parent, "(ColorFilterEffect missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        auto* blendField = InspectorUI::AddEnumRow(ctx.Parent, "BlendMode", kBlendModes, effect->BlendMode,
            "How tint is combined with scene color");
        blendField->SetOnValueChanged([w, e, n, undo](ColorFilterBlendMode v) {
            CommitComponentWithUndo<CF>(w, e, n, undo, "Change ColorFilter Blend Mode",
                [v](CF& u) { u.BlendMode = v; });
        });

        // Color swatch + RGB label, click opens color picker.
        {
            const InspectorUI::ColorSwatchRow colorRow = InspectorUI::AddColorSwatchRow(
                ctx.Parent, "Color", "Tint color (click to open color picker; double-click to reset to white)",
                ColorUtils::RgbFloatsToArgb(effect->Color),
                InspectorUI::FormatColorRgb(effect->Color[0], effect->Color[1], effect->Color[2]));
            Label* colorLabel = colorRow.Title;
            UIElement* swatchRaw = colorRow.Swatch;
            Label* rgbLabelRaw = colorRow.Value;

            // The picker's callbacks outlive an inspector rebuild: resolve the row through
            // weak refs instead of holding the freed widgets.
            auto clickHandler = [w, e, n, undo, openPicker,
                                 swatchRef = UIElement::MakeWeakRef(swatchRaw),
                                 rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw)](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();

                if (!openPicker)
                    return;

                auto* comp = w->GetComponent<CF>(e);
                if (!comp)
                    return;

                uint32_t currentArgb = ColorUtils::RgbFloatsToArgb(comp->Color);

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();

                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<CF>(w, e, n, "ColorFilter Color");
                    *edit = undo->BeginInteractiveEdit("Change ColorFilter Color", std::move(target));
                }

                auto updateUI = [swatchRef, rgbLabelRef, w, e]() {
                    auto* c = w->GetComponent<CF>(e);
                    UIElement* swatch = swatchRef.Get();
                    Label* rgbLabel = rgbLabelRef.Get();
                    if (!c || !swatch || !rgbLabel) return;
                    InspectorUI::SetColorSwatchColor(swatch, ColorUtils::RgbFloatsToArgb(c->Color));
                    rgbLabel->SetText(InspectorUI::FormatColorRgb(c->Color[0], c->Color[1], c->Color[2]));
                };

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<CF>(e);
                            if (c) ColorUtils::ArgbToRgbFloats(newArgb, c->Color);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<CF>(e);
                        if (!c) return;
                        CF updated = *c;
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
                            auto* c = w->GetComponentForWrite<CF>(e);
                            if (c) ColorUtils::ArgbToRgbFloats(newArgb, c->Color);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<CF>(e);
                        if (!c) return;
                        CF updated = *c;
                        ColorUtils::ArgbToRgbFloats(newArgb, updated.Color);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(currentArgb, 1.0f, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

            // Double-click on the "Color" label resets to white.
            auto lastColorClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
            colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, lastColorClickTime](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastColorClickTime);
                if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                {
                    ev.Stop();
                    CommitComponentWithUndo<CF>(w, e, n, undo, "Reset ColorFilter Color",
                        [](CF& u) { u.Color[0] = 1.0f; u.Color[1] = 1.0f; u.Color[2] = 1.0f; });
                    *lastColorClickTime = std::chrono::steady_clock::time_point{};
                    return;
                }
                *lastColorClickTime = now;
            });
        }

        AddComponentFloatRowWithDrag<CF>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change ColorFilter Intensity",
            [](CF& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            1.0f, "Blend amount: 0 = pass-through, 1 = full tint");
    };

    InspectorRegistry::Get().RegisterComponentInspector<CF>(std::move(fn));
}

} // namespace GameEngine
