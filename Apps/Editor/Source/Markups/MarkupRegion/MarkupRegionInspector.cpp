#include "Markups/MarkupRegion/MarkupRegionInspector.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/ConvertMarkupShapeCommand.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspectorRows.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegion/MarkupRegionPort.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "Types/FormatNumber.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/EntityMemberList/EntityMemberList.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Components::MarkupRegionMember;

// Areas from this many square meters up read in hectares.
constexpr float32 kHectare = 10000.0f;
// A perimeter reads to a tenth of a meter.
constexpr int kLengthDecimals = 1;

std::string FormatArea(float32 squareMeters)
{
    if (squareMeters >= kHectare)
        return FormatFixed(squareMeters / kHectare, 2) + " ha";
    return FormatFixed(squareMeters, 0) + " m\xC2\xB2";
}

using Edit = UndoRedoService::InteractiveEdit;

// The height row's slider and the drag it holds (empty between drags).
struct HeightRow
{
    Slider* Control = nullptr;
    FloatField* Value = nullptr;
    std::shared_ptr<Edit> Drag;
};

// The height: previewed on the region every frame of a drag, one undo step on release.
HeightRow AddHeightRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupRegion& region)
{
    auto edit = std::make_shared<Edit>();
    const auto write = [ctx](float32 height) {
        if (MarkupRegion* edited = ctx.World->GetComponentForWrite<MarkupRegion>(ctx.Entity))
            edited->ExtrudeHeight = std::clamp(height, kMinExtrudeHeight, kMaxExtrudeHeight);
    };
    const auto changing = [ctx, &bridge, edit, write](float height) {
        if (bridge.IsInPlayMode())
            return;
        if (!*edit && ctx.Undo)
            *edit = ctx.Undo->BeginInteractiveEdit(
                "Set Region Height", InspectorDrag::MakeComponentSnapshotTarget<MarkupRegion>(
                                         ctx.World, ctx.Entity, ctx.ChangeNotifications, "Region Height"));
        if (*edit)
            edit->Preview([&] { write(height); });
    };
    const auto changed = [ctx, &bridge, edit, write](float height) {
        if (bridge.IsInPlayMode())
            return;
        if (*edit)
        {
            edit->Preview([&] { write(height); });
            edit->Commit();
            *edit = Edit{};
            return;
        }
        InspectorDrag::CommitComponentWithUndo<MarkupRegion>(
            ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Set Region Height",
            [height](MarkupRegion& edited) {
                edited.ExtrudeHeight = std::clamp(height, kMinExtrudeHeight, kMaxExtrudeHeight);
            });
    };
    const InspectorDrag::SliderWithFloatValueRow row = InspectorDrag::AddFloatSliderRow(
        ctx.Parent, "Height", region.ExtrudeHeight, kMinExtrudeHeight, kMaxExtrudeHeight, changing, changed,
        "How tall the walls stand above the ground, in meters", "m");
    row.Slider->AddClass("markup-region-height");
    // A drag lands between whole meters; the field reads it to a tenth.
    row.ValueField->SetFixedDecimalPlaces(1);
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(row.Slider, false);
    return HeightRow{row.Slider, row.ValueField, edit};
}

// The outline's knot count, perimeter and area (members not applied: the base outline's).
void AddOutlineRows(const InspectorContext& ctx)
{
    const auto* spline = ctx.World->GetComponent<Components::SplineComponent>(ctx.Entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    const Spline::SplineData* data =
        spline && splines
            ? splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
            : nullptr;
    AddValueRow(ctx.Parent, "Points", std::to_string(data ? data->Points.size() : 0u),
                "The outline's knots; Edit Outline moves, adds and deletes them");
    const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(*ctx.World, ctx.Entity);
    if (!footprint || footprint->IsCircle())
        return;
    AddValueRow(ctx.Parent, "Perimeter", FormatMeters(MarkupECS::RegionOutlinePerimeter(footprint->Ring), kLengthDecimals),
                "The outline's length on the ground plane");
    AddValueRow(ctx.Parent, "Outline area", FormatArea(MarkupECS::RegionOutlineArea(footprint->Ring)),
                "The area the outline encloses on the ground plane, before its members apply");
}

// The region's members as they are now.
std::vector<MarkupRegionMember> CurrentMembers(const MarkupRegion& region)
{
    return std::vector<MarkupRegionMember>(region.Members,
                                           region.Members + std::min(region.MemberCount, Components::kMaxRegionMembers));
}

// Writes `members` as the region's list, one undo step.
void CommitMembers(const InspectorContext& ctx, const std::vector<MarkupRegionMember>& members, const char* label)
{
    InspectorDrag::CommitComponentWithUndo<MarkupRegion>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, label, [&members](MarkupRegion& edited) {
            edited.MemberCount = static_cast<uint32>(std::min<std::size_t>(members.size(), Components::kMaxRegionMembers));
            std::fill(std::begin(edited.Members), std::end(edited.Members), MarkupRegionMember{});
            std::copy_n(members.begin(), edited.MemberCount, edited.Members);
        });
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

bool BoundsOverlap(const MarkupECS::MarkupFootprint& a, const MarkupECS::MarkupFootprint& b)
{
    return a.Min.x <= b.Max.x && b.Min.x <= a.Max.x && a.Min.y <= b.Max.y && b.Min.y <= a.Max.y;
}

// A predicate that is true for the member entry of `entity`.
auto IsMemberEntry(ECS::EntityHandle entity)
{
    return [entity](const MarkupRegionMember& member) { return member.Entity == entity; };
}

// What the row says beside the title of a live mark-up: that it is outside the region, or a region
// member's base outline rule.
std::string MemberNote(const InspectorContext& ctx, ECS::EntityHandle entity, bool overlaps)
{
    if (!overlaps)
        return "outside the region";
    if (const auto* memberRegion = ctx.World->GetComponent<MarkupRegion>(entity);
        memberRegion && memberRegion->MemberCount > 0)
        return "base outline only: its own " + std::to_string(memberRegion->MemberCount) + " members do not apply here";
    if (ctx.World->GetComponent<MarkupRegion>(entity))
        return "base outline only";
    return {};
}

// The Include | Exclude choice every row of the region offers. Exclude takes area away.
std::vector<EditorUI::EntityMemberList::Mode> RegionMemberModes()
{
    return {
        {static_cast<uint8>(MarkupMemberMode::Include), "Include", "Include adds its footprint to the region's area",
         false},
        {static_cast<uint8>(MarkupMemberMode::Exclude), "Exclude", "Exclude cuts its footprint out of the region's area",
         true},
    };
}

// One mark-up the list offers. `footprint` is empty for a deleted member.
EditorUI::EntityMemberList::Row MemberRow(const InspectorContext& ctx, ECS::EntityHandle entity,
                                          const std::optional<MarkupECS::MarkupFootprint>& footprint,
                                          const std::optional<MarkupECS::MarkupFootprint>& base)
{
    EditorUI::EntityMemberList::Row row;
    row.Entity = entity;
    row.Live = footprint.has_value();
    const MarkupKind kind = row.Live ? ReadMarkupKind(*ctx.World, entity) : MarkupKind::None;
    row.GlyphClasses = MarkupKindGlyphClasses(kind);
    row.GlyphTooltip = MarkupKindName(kind);
    row.Modes = RegionMemberModes();
    if (row.Live)
    {
        row.Title = MarkupTitle(*ctx.World, entity);
        row.Note = MemberNote(ctx, entity, base && BoundsOverlap(*base, *footprint));
    }
    return row;
}

// The members, in their order, then every other mark-up whose footprint overlaps the region.
EditorUI::EntityMemberList::Content CollectMemberRows(const InspectorContext& ctx, MarkupEditorBridge& bridge,
                                                      const MarkupRegion& region)
{
    EditorUI::EntityMemberList::Content content;
    content.Text.Heading = "Members";
    content.Text.HeadingTooltip = "Inside the region = its outline or an Include member, and not an Exclude member";
    content.Text.Empty = "No other mark-up overlaps this region";
    content.Text.DeletedTitle = "Deleted mark-up";
    content.Text.MemberTooltip = "A member: uncheck to take it out of the region's area";
    content.Text.CandidateTooltip = "Check to make it a member of the region's area";
    content.Editable = !bridge.IsInPlayMode();
    const std::vector<MarkupRegionMember> members = CurrentMembers(region);
    if (members.size() >= Components::kMaxRegionMembers)
        content.AddRefusal = "A region holds at most " + std::to_string(Components::kMaxRegionMembers) +
                             " members; take one out first";
    const std::optional<MarkupECS::MarkupFootprint> base = MarkupECS::ReadMarkupFootprint(*ctx.World, ctx.Entity);
    for (const MarkupRegionMember& member : members)
    {
        EditorUI::EntityMemberList::Row& row = content.Rows.emplace_back(
            MemberRow(ctx, member.Entity, MarkupECS::ReadMarkupFootprint(*ctx.World, member.Entity), base));
        row.On = true;
        row.Member = true;
        row.CurrentMode = static_cast<uint8>(member.Mode);
    }
    if (!base)
        return content;
    ctx.World->Query<ECS::Read<Components::Markup>>().Each(
        [&](ECS::EntityHandle entity, const Components::Markup&) {
            if (entity == ctx.Entity || std::any_of(members.begin(), members.end(), IsMemberEntry(entity)))
                return;
            const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(*ctx.World, entity);
            if (footprint && BoundsOverlap(*base, *footprint))
                content.Rows.push_back(MemberRow(ctx, entity, footprint, base));
        });
    return content;
}

// The region's members now, or empty when the entity holds no region.
std::vector<MarkupRegionMember> CurrentMembersOf(const InspectorContext& ctx)
{
    const MarkupRegion* region = ctx.World->GetComponent<MarkupRegion>(ctx.Entity);
    return region ? CurrentMembers(*region) : std::vector<MarkupRegionMember>{};
}

std::vector<MarkupRegionMember>::iterator FindMember(std::vector<MarkupRegionMember>& members, ECS::EntityHandle entity)
{
    return std::find_if(members.begin(), members.end(), IsMemberEntry(entity));
}

void AddMember(const InspectorContext& ctx, ECS::EntityHandle entity)
{
    std::vector<MarkupRegionMember> members = CurrentMembersOf(ctx);
    if (FindMember(members, entity) != members.end())
        return;
    members.push_back({entity, MarkupMemberMode::Include});
    CommitMembers(ctx, members, "Add Region Member");
}

void RemoveMember(const InspectorContext& ctx, ECS::EntityHandle entity)
{
    std::vector<MarkupRegionMember> members = CurrentMembersOf(ctx);
    const auto found = FindMember(members, entity);
    if (found == members.end())
        return;
    members.erase(found);
    CommitMembers(ctx, members, "Remove Region Member");
}

void SwitchMember(const InspectorContext& ctx, ECS::EntityHandle entity, uint8 mode)
{
    std::vector<MarkupRegionMember> members = CurrentMembersOf(ctx);
    const auto found = FindMember(members, entity);
    if (found == members.end() || found->Mode == static_cast<MarkupMemberMode>(mode))
        return;
    found->Mode = static_cast<MarkupMemberMode>(mode);
    CommitMembers(ctx, members, "Switch Region Member");
}

// The member list: each gesture is one undo step that writes the region's list.
void AddMemberList(const InspectorContext& ctx, MarkupEditorBridge& bridge, const MarkupRegion& region)
{
    EditorUI::EntityMemberList::Handlers handlers;
    handlers.SetOn = [ctx](ECS::EntityHandle entity, bool on) { on ? AddMember(ctx, entity) : RemoveMember(ctx, entity); };
    handlers.SetMode = [ctx](ECS::EntityHandle entity, uint8 mode) { SwitchMember(ctx, entity, mode); };
    handlers.Select = [world = ctx.World, &bridge](ECS::EntityHandle entity) { bridge.SelectMarkup(*world, entity); };
    ctx.Parent->AddChild(
        std::make_unique<EditorUI::EntityMemberList>(CollectMemberRows(ctx, bridge, region), handlers));
}

// The Height row follows the region's height while no drag holds it.
std::function<void()> FollowHeight(const InspectorContext& ctx, const HeightRow& height)
{
    return [world = ctx.World, entity = ctx.Entity, height]() {
        if (const auto* region = world->GetComponent<MarkupRegion>(entity); region && !*height.Drag)
        {
            height.Control->SetValueWithoutNotify(region->ExtrudeHeight);
            height.Value->SetValueWithoutNotify(region->ExtrudeHeight);
        }
    };
}

} // namespace

void AddMarkupRegionBlock(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;
    const MarkupRegion* region = ctx.World->GetComponent<MarkupRegion>(ctx.Entity);
    if (!region)
        return;
    auto title = std::make_unique<Label>();
    title->AddClass("markup-region-title");
    title->SetText("Region");
    ctx.Parent->AddChild(std::move(title));
    if (const RegionOutlineProblem problem = ReadRegionOutlineProblem(*ctx.World, ctx.Entity);
        problem != RegionOutlineProblem::None)
        ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
            RegionOutlineProblemFix(problem), EditorUI::InspectorNotice::Kind::Warning));
    const HeightRow height = AddHeightRow(ctx, bridge, *region);
    AddOutlineRows(ctx);
    AddEditKnotsRow(ctx, bridge, "Edit Outline", "markup-region-edit-outline",
                    "Drag, add and delete the outline's points in the Scene View");
    AddMemberList(ctx, bridge, *region);
    FollowMarkupShapeChanges(ctx, FollowHeight(ctx, height));
}

bool ConvertBoxMarkupToRegion(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                              EditorChangeNotifications* notifications)
{
    std::optional<BoxRegionShape> box = BoxAsRegion(world, entity);
    if (!box)
        return false;
    auto command = std::make_unique<ConvertMarkupShapeCommand>(
        kConvertToRegionLabel, world, entity, notifications,
        ConvertedRegion{std::move(box->Knots), Spline::SplineType::Linear, box->ExtrudeHeight});
    if (undo)
        undo->Execute(std::move(command));
    else
        command->Do();
    return true;
}

void AddConvertToRegionRow(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    const auto* volume = ctx.World->GetComponent<Components::MarkupVolume>(ctx.Entity);
    if (!volume || volume->Shape != Components::MarkupVolumeShape::Box)
        return;
    UIElement* cell = InspectorUI::AddActionRow(ctx.Parent, "markup-region-actions");
    auto button = std::make_unique<Button>();
    button->SetText("Convert to Region");
    button->AddClass("small");
    button->AddClass("markup-convert-to-region");
    button->SetTooltip("Make this box a region with the box's outline and height; its notes and thread stay");
    button->RegisterEventHandler(kEventButtonClick, [ctx, &bridge](UIEvent&) {
        if (bridge.IsInPlayMode())
            return;
        if (ConvertBoxMarkupToRegion(*ctx.World, ctx.Entity, ctx.Undo, ctx.ChangeNotifications) &&
            ctx.RequestInspectorRefresh)
            ctx.RequestInspectorRefresh();
    });
    Button* raw = button.get();
    cell->AddChild(std::move(button));
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(raw, false);
}

void RegisterMarkupRegionTraits()
{
    EditorComponentTraits traits;
    const ECS::ComponentTypeId type = ECS::GetComponentTypeId<MarkupRegion>();
    EditorComponentTraitsRegistry::Get().TryGet(type, traits);
    traits.DisplayName = "Mark-up Region";
    // The Mark-up section holds the Region block (AddMarkupRegionBlock).
    traits.HideInInspector = true;
    // A region's entity is placed at its label point (NewRegionPlacement): the move gizmo pivots
    // at the entity, inside the region, not at the spline's first knot on its edge.
    traits.GizmoPivotAtEntityOrigin = true;
    EditorComponentTraitsRegistry::Get().Register(type, std::move(traits));
}

} // namespace GameEngine::Editor
