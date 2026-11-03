#include "Inspectors/TileMapLayerInspector.h"

#include "Rendering/Sorting/RenderSortingEditor.h"

#include "InspectorRegistry.h"
#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Assets/AssetManager.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

using TileMapLayerRef = Components::TileMapLayerRef;

constexpr float32 kSwatchSize = 18.0f;
constexpr float32 kSwatchBorderRadius = 3.0f;
constexpr uint32 kSwatchBorderColor = 0xFF555555u;
constexpr float32 kSwatchGapPx = 6.0f;

constexpr float32 kOpacityMin = 0.0f;
constexpr float32 kOpacityMax = 1.0f;
// Parallax below 0 would scroll the layer against the camera, which reads as a
// bug rather than an effect; above 1 is a valid foreground layer.
constexpr float32 kParallaxMin = 0.0f;
constexpr float32 kParallaxMax = 4.0f;
// Neutral values the field's double-click reset returns to.
constexpr float32 kParallaxDefault = 1.0f;
constexpr float32 kZOffsetDefault = 0.0f;

/** The component stores tint as 0xRRGGBBAA; UI swatches want 0xAARRGGBB. */
uint32 TintToArgb(uint32 rgba)
{
    const uint32 r = (rgba >> 24) & 0xFFu;
    const uint32 g = (rgba >> 16) & 0xFFu;
    const uint32 b = (rgba >> 8) & 0xFFu;
    const uint32 a = rgba & 0xFFu;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32 ArgbToTint(uint32 argb)
{
    const uint32 a = (argb >> 24) & 0xFFu;
    const uint32 r = (argb >> 16) & 0xFFu;
    const uint32 g = (argb >> 8) & 0xFFu;
    const uint32 b = argb & 0xFFu;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

void StyleSwatch(UIElement* swatch, uint32 argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kSwatchSize))
        .Set(Style::Height, StyleLength::Px(kSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{kSwatchBorderRadius, kSwatchBorderRadius,
                                                      kSwatchBorderRadius, kSwatchBorderRadius})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kSwatchBorderColor, kSwatchBorderColor,
                                                  kSwatchBorderColor, kSwatchBorderColor})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

/** Layer options for the dropdown, paired with the stable ids they select. */
struct LayerOptions
{
    std::vector<Dropdown::Option> Options;
    std::vector<uint32> LayerIds;
    int32 SelectedIndex = -1;
};

LayerOptions BuildLayerOptions(const GUID& mapGuid, uint32 selectedLayerId)
{
    LayerOptions out;
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!store || mapGuid.IsNull())
        return out;
    const TileMap::TileMapData* data = store->GetMap(mapGuid);
    if (!data)
        return out;

    for (const TileMap::TileMapLayerData& layer : data->Layers())
    {
        if (layer.Id == selectedLayerId)
            out.SelectedIndex = static_cast<int32>(out.LayerIds.size());
        const std::string label =
            layer.Name.empty() ? ("Layer " + std::to_string(layer.Id)) : layer.Name;
        out.Options.push_back(Dropdown::Option{.value = std::to_string(layer.Id), .label = label});
        out.LayerIds.push_back(layer.Id);
    }
    return out;
}

void AddTintRow(const InspectorContext& ctx, ECS::World* w, ECS::EntityHandle e,
                Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo)
{
    using namespace InspectorDrag;

    const TileMapLayerRef* layerRef = w->GetComponent<TileMapLayerRef>(e);
    if (!layerRef)
        return;

    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    if (Label* label = InspectorUI::AddLabel(row, "Tint", "Multiplied into every tile's texel color."))
        label->AddClass("inspector-label-no-drag");

    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(kSwatchGapPx));

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleSwatch(swatchRaw, TintToArgb(layerRef->TintRGBA));
    fieldContainer->AddChild(std::move(swatch));

    auto updateSwatch = [swatchRaw, w, e]()
    {
        if (const auto* current = w->GetComponent<TileMapLayerRef>(e))
            StyleSwatch(swatchRaw, TintToArgb(current->TintRGBA));
    };

    OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;
    swatchRaw->RegisterEventHandler(
        kEventMouseDown,
        [w, e, n, undo, openPicker, updateSwatch](UIEvent& ev)
        {
            if (ev.Button != 0 || !openPicker)
                return;
            ev.Stop();

            const auto* comp = w->GetComponent<TileMapLayerRef>(e);
            if (!comp)
                return;

            using Edit = Editor::UndoRedoService::InteractiveEdit;
            auto edit = std::make_shared<Edit>();
            if (undo)
            {
                auto target = MakeComponentSnapshotTarget<TileMapLayerRef>(w, e, n, "Layer Tint");
                *edit = undo->BeginInteractiveEdit("Change Tile Layer Tint", std::move(target));
            }

            auto writeTint = [w, e, n, edit](uint32 argb, bool commit)
            {
                if (*edit)
                {
                    edit->Preview(
                        [&]
                        {
                            if (auto* c = w->GetComponentForWrite<TileMapLayerRef>(e))
                                c->TintRGBA = ArgbToTint(argb);
                        });
                    if (commit)
                        edit->Commit();
                    return;
                }
                const auto* c = w->GetComponent<TileMapLayerRef>(e);
                if (!c)
                    return;
                TileMapLayerRef updated = *c;
                updated.TintRGBA = ArgbToTint(argb);
                if (commit)
                    Editor::CommitComponentUpdate(w, e, n, updated);
                else
                    Editor::PreviewComponentUpdate(w, e, n, updated);
            };

            ColorPickerCallbacks callbacks;
            callbacks.onApply = [writeTint, updateSwatch](uint32 argb, float)
            {
                writeTint(argb, true);
                updateSwatch();
            };
            callbacks.onValueChanging = [writeTint, updateSwatch](uint32 argb, float)
            {
                writeTint(argb, false);
                updateSwatch();
            };
            callbacks.onCancel = [edit, updateSwatch]()
            {
                if (*edit)
                    edit->Cancel();
                updateSwatch();
            };
            openPicker(TintToArgb(comp->TintRGBA), 1.0f, callbacks);
        });
}

} // namespace

void RegisterTileMapLayerInspector()
{
    Editor::RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::TileMapLayerRef>());
    using namespace InspectorDrag;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        const TileMapLayerRef* layerRef = ctx.World->GetComponent<TileMapLayerRef>(ctx.Entity);
        if (!layerRef)
            return;

        Editor::AddRenderSortingDiagnostic(ctx);

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        InspectorUI::AddAssetFieldRow(
            ctx.Parent, "Tile Map", layerRef->TileMap.ToGuid(), {AssetType::TileMap},
            (ctx.EditorCtx && ctx.EditorCtx->Assets) ? &ctx.EditorCtx->Assets->GetRegistry()
                                                      : nullptr,
            [w, e, n, undo](const GUID& guid)
            {
                CommitComponentWithUndo<TileMapLayerRef>(
                    w, e, n, undo, "Change Tile Map",
                    [guid](TileMapLayerRef& updated)
                    {
                        updated.TileMap = Components::AssetRef<AssetType::TileMap>(guid);
                        // Layer ids are only meaningful inside one map, so a new
                        // map leaves the reference unset rather than pointing at
                        // an unrelated layer that happens to share the id.
                        updated.LayerId = 0;
                    });
            },
            ctx.Thumbnails, "The .tilemap asset this entity renders a layer of.");

        const LayerOptions layers = BuildLayerOptions(layerRef->TileMap.ToGuid(), layerRef->LayerId);
        if (layers.Options.empty())
        {
            InspectorUI::AddLine(ctx.Parent,
                                 "(No layers — pick a tilemap, or add a layer in Tile Palette)");
        }
        else
        {
            Dropdown* layerDropdown =
                InspectorUI::AddDropdownRow(ctx.Parent, "Layer", layers.Options, layers.SelectedIndex,
                               "Which layer of the tilemap this entity renders.");
            // The dropdown reports the selected label, but layer names are not
            // unique — resolve through the index so two layers sharing a name
            // still select the right id.
            layerDropdown->SetOnValueChanged(
                [w, e, n, undo, layerDropdown, ids = layers.LayerIds](const std::string&)
                {
                    const int index = layerDropdown->GetSelectedIndex();
                    if (index < 0 || static_cast<size_t>(index) >= ids.size())
                        return;
                    const uint32 layerId = ids[static_cast<size_t>(index)];
                    CommitComponentWithUndo<TileMapLayerRef>(
                        w, e, n, undo, "Change Tile Layer",
                        [layerId](TileMapLayerRef& updated) { updated.LayerId = layerId; });
                });
        }

        AddToggleRow(
            ctx.Parent, "Visible", layerRef->Visible,
            [w, e, n, undo](bool value)
            {
                CommitComponentWithUndo<TileMapLayerRef>(
                    w, e, n, undo, "Toggle Tile Layer Visibility",
                    [value](TileMapLayerRef& updated) { updated.Visible = value; });
            },
            "Hides the layer without removing it from the scene.");

        AddComponentFloatSliderRow<TileMapLayerRef>(
            ctx.Parent, "Opacity", layerRef->Opacity, kOpacityMin, kOpacityMax, w, e, n,
            [](TileMapLayerRef& updated, float32 value) { updated.Opacity = value; },
            "Layer alpha, multiplied with the tint's alpha.");

        AddTintRow(ctx, w, e, n, undo);

        AddComponentFloatRowWithDrag<TileMapLayerRef>(
            ctx.Parent, "Parallax X", layerRef->ParallaxX, w, e, n, undo, "Change Tile Layer Parallax",
            [](TileMapLayerRef& updated, float32 value) { updated.ParallaxX = value; },
            kParallaxDefault,
            "Fraction of camera motion the layer follows: 1 locks it to the world, 0 to the camera.",
            {}, kParallaxMin, kParallaxMax);

        AddComponentFloatRowWithDrag<TileMapLayerRef>(
            ctx.Parent, "Parallax Y", layerRef->ParallaxY, w, e, n, undo, "Change Tile Layer Parallax",
            [](TileMapLayerRef& updated, float32 value) { updated.ParallaxY = value; },
            kParallaxDefault, "Fraction of camera motion the layer follows on the vertical axis.",
            {}, kParallaxMin, kParallaxMax);

        AddComponentFloatRowWithDrag<TileMapLayerRef>(
            ctx.Parent, "Z Offset", layerRef->ZOffset, w, e, n, undo, "Change Tile Layer Z Offset",
            [](TileMapLayerRef& updated, float32 value) { updated.ZOffset = value; },
            kZOffsetDefault,
            "Local depth offset. Camera-relative depth resolves draws within the same sorting layer and order.");
    };

    InspectorRegistry::Get().RegisterComponentInspector<TileMapLayerRef>(std::move(fn));
}

} // namespace GameEngine
