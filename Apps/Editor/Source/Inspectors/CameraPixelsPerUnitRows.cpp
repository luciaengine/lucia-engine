#include "Inspectors/CameraInspector.h"

#include "Components/Rendering/Camera.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TileMapECS/TileMapPixelGrid.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

std::string ResolvedPixelsPerUnitText(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* camera = world.GetComponent<Components::Camera>(entity);
    if (!camera)
        return {};
    const uint32 effective = Engine::Renderer::ResolvePixelPerfectPixelsPerUnit(world, *camera);
    const bool fromTileSet = TileMapECS::ResolveScenePixelsPerUnit(world) > 0.0f;
    return std::to_string(effective) + (fromTileSet ? " (from tile set)" : " (typed value)");
}

void RefreshResolvedPixelsPerUnit(Label& shown, ECS::World& world, ECS::EntityHandle entity)
{
    std::string text = ResolvedPixelsPerUnitText(world, entity);
    if (text != shown.GetText())
        shown.SetText(text);
}

void AddResolvedPixelsPerUnitRow(const InspectorContext& ctx)
{
    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    InspectorUI::AddLabel(row, "Pixels Per Unit",
                          "Source texels per world unit, from the tile set of the lowest visible tile map layer. "
                          "Turn off Auto Pixels Per Unit to type a value.");
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->AddClass("inspector-field-readout");
    Label* shown = InspectorUI::AddTextBlock(field, ResolvedPixelsPerUnitText(*ctx.World, ctx.Entity));
    if (shown && ctx.FrameRefreshCallbacks)
        ctx.FrameRefreshCallbacks->push_back([shown, world = ctx.World, entity = ctx.Entity]()
                                             { RefreshResolvedPixelsPerUnit(*shown, *world, entity); });
}

void AddTypedPixelsPerUnitRow(const InspectorContext& ctx, const Components::Camera& camera)
{
    const int value = static_cast<int>(camera.PixelPerfectPixelsPerUnit);
    InspectorDrag::AddComponentIntRowWithDrag<Components::Camera>(
        ctx.Parent, "Pixels Per Unit", value, ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
        "Change Camera Pixels Per Unit",
        [](Components::Camera& u, int v) { u.PixelPerfectPixelsPerUnit = static_cast<uint32>(std::max(1, v)); },
        value, "Reference pixel size: source texels per world unit", InspectorDrag::GetAdditionalEntities(ctx));
}

} // namespace

void AddCameraPixelsPerUnitRows(const InspectorContext& ctx, std::function<void()> requestRebuild)
{
    if (!ctx.World)
        return;
    const auto* camera = ctx.World->GetComponent<Components::Camera>(ctx.Entity);
    if (!camera)
        return;

    InspectorDrag::AddToggleRow(ctx.Parent, "Auto Pixels Per Unit", camera->PixelPerfectAutoPixelsPerUnit,
        [world = ctx.World, entity = ctx.Entity, extras = InspectorDrag::GetAdditionalEntities(ctx), notifications = ctx.ChangeNotifications,
         undo = ctx.Undo, requestRebuild = std::move(requestRebuild)](bool on)
        {
            InspectorDrag::CommitComponentsWithUndo<Components::Camera>(world, entity, extras, notifications, undo,
                "Change Camera Auto Pixels Per Unit",
                [on](Components::Camera& u) { u.PixelPerfectAutoPixelsPerUnit = on; });
            if (requestRebuild)
                requestRebuild();
        },
        "Take Pixels Per Unit from the scene's tile maps (tile pixel width / cell width). With no tile map, "
        "the camera uses the value you typed.");

    if (camera->PixelPerfectAutoPixelsPerUnit)
        AddResolvedPixelsPerUnitRow(ctx);
    else
        AddTypedPixelsPerUnitRow(ctx, *camera);
}

} // namespace GameEngine
