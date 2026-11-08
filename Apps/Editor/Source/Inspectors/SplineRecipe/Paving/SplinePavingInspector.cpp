#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Spline/SplineData.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "SplineECS/SplineService.h"
#include "SplineNetworkRecordCommand.h"
#include "UI/Controls/Button.h"
#include "UndoRedo/UndoRedoService.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <algorithm>

namespace GameEngine
{
namespace
{

using Components::SplinePaving;
using Components::SplinePavingMemberMode;

void WriteMember(const InspectorContext& ctx, ECS::EntityHandle spline, SplinePavingMemberMode mode, bool include)
{
    if (!ctx.World->IsValid(spline))
        return;
    InspectorDrag::CommitComponentWithUndo<SplinePaving>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Change Paving Members",
        [spline, mode, include](SplinePaving& recipe)
        {
            recipe.MemberCount = std::min(recipe.MemberCount, Components::kMaxPavingMembers);
            auto* end = recipe.Members + recipe.MemberCount;
            auto* found = std::find_if(recipe.Members, end, [spline](const auto& member)
            {
                return member.Entity == spline;
            });
            if (found != end && include)
            {
                std::move(found + 1, end, found);
                recipe.Members[--recipe.MemberCount] = {};
            }
            else if (found == end && !include && recipe.MemberCount < Components::kMaxPavingMembers)
            {
                recipe.Members[recipe.MemberCount++] = {spline, mode};
            }
        });
}

void AddMemberRows(const InspectorContext& ctx, const SplinePaving& recipe)
{
    const auto* service = SplineECS::SplineService::TryGet();
    if (!service)
        return;

    const auto* end = recipe.Members + std::min(recipe.MemberCount, Components::kMaxPavingMembers);
    ctx.World->Query<ECS::Read<Components::SplineComponent>>().Each(
        [&](ECS::EntityHandle spline, const Components::SplineComponent& component)
        {
            if (spline == ctx.Entity)
                return;
            const auto* network = ctx.World->GetComponent<Components::SplineNetworkMember>(spline);
            const auto* data = service->GetSplineData(SplineECS::SplineHandle{component.SplineDataIndex, component.SplineDataGeneration});
            const bool road = network && network->Kind == Components::SplineNetworkMemberKind::Road;
            const bool member = std::any_of(recipe.Members, end, [spline](const auto& entry)
            {
                return entry.Entity == spline;
            });
            if (!member && !road && (!data || !data->IsEffectivelyClosed()))
                return;

            const auto mode = road ? SplinePavingMemberMode::Ignore : SplinePavingMemberMode::Exclude;
            const std::vector<Dropdown::Option> options{{"0", "Include", {}, {}}, {"1", road ? "Ignore Road" : "Cut Out", {}, {}}};
            auto* field = InspectorUI::AddDropdownRow(ctx.Parent, Editor::EntityDisplayName(*ctx.World, spline),
                                                      options, member ? 1 : 0);
            field->SetOnValueChanged([ctx, spline, mode](const std::string& value)
            {
                WriteMember(ctx, spline, mode, value == "0");
            });
        });
}

void AddJunctionRow(const InspectorContext& ctx, const SplineRecipes::PavingJunctionStatus& junction)
{
    using SplineRecipes::PavingJunctionState;
    if (junction.State != PavingJunctionState::HeightsDiffer && junction.State != PavingJunctionState::Paved)
        return;
    const auto* network = SplineECS::SplineNetwork::TryGet();
    const auto* node = network ? network->FindNode(*ctx.World, junction.NodeId) : nullptr;
    const bool clear = node && node->Rule == Components::SplineNodeRule::AtGrade;
    if (!clear && junction.State != PavingJunctionState::HeightsDiffer)
        return;

    auto* row = InspectorUI::AddRow(ctx.Parent);
    InspectorUI::AddLabel(row, Editor::EntityDisplayName(*ctx.World, junction.Road));
    auto button = std::make_unique<Button>();
    button->AddClass("inspector-button");
    button->SetText(clear ? "Clear Grade Override" : "Join at Grade");
    button->RegisterEventHandler(kEventButtonClick, [ctx, junction, clear](UIEvent&)
    {
        const auto* currentNetwork = SplineECS::SplineNetwork::TryGet();
        const auto* current = currentNetwork ? currentNetwork->FindNode(*ctx.World, junction.NodeId) : nullptr;
        if (!current || (clear && current->Rule != Components::SplineNodeRule::AtGrade))
            return;
        auto command = clear
            ? Editor::SplineNetworkRecordCommand::Clear(*ctx.World, current->RuleHolder, current->RuleSlot)
            : Editor::SplineNetworkRecordCommand::JoinAtGrade(*ctx.World, *current, junction.Road);
        if (!command)
            return;
        if (ctx.Undo)
            ctx.Undo->Execute(std::move(command));
        else
            command->Do();
        if (ctx.RequestInspectorRefresh)
            ctx.RequestInspectorRefresh();
    });
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(button));
}

const SplineRecipes::RecipeControllers* CurrentRecipes()
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    const auto* recipes = systems ? systems->GetSystem<SplineRecipes::SplineRecipeSystem>() : nullptr;
    return recipes ? &recipes->GetControllers() : nullptr;
}

void BuildExtrudeInspector(const InspectorContext& ctx)
{
    using Components::SplineExtrude;
    const auto* recipe = ctx.World ? ctx.World->GetComponent<SplineExtrude>(ctx.Entity) : nullptr;
    if (!recipe || !ctx.Parent)
        return;

    constexpr auto typeId = ECS::GetComponentTypeId<SplineExtrude>();
    std::vector<std::string_view> fields;
    for (const auto& field : ECS::ComponentFieldRegistry::Get(typeId))
        if (field.Name != std::string_view("Verge"))
            fields.emplace_back(field.Name);
    RenderReflectedFieldRows(ctx, typeId, fields);

    auto* row = InspectorUI::AddRow(ctx.Parent);
    InspectorUI::AddLabel(row, "Build Verge", "Adds a curb to both sides of a Channel extrude.");
    auto* toggle = InspectorUI::AddToggle(InspectorUI::AddFieldContainer(row), recipe->Verge);
    toggle->SetOnValueChanged([ctx](bool enabled)
    {
        InspectorDrag::CommitComponentWithUndo<SplineExtrude>(
            ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Change Road Verge",
            [ctx, enabled](SplineExtrude& updated)
            {
                updated.Verge = enabled;
                if (const auto* recipes = enabled ? CurrentRecipes() : nullptr)
                    recipes->PrefillVergeMaterial(*ctx.World, ctx.Entity, updated);
            });
        if (ctx.RequestInspectorRefresh)
            ctx.RequestInspectorRefresh();
    });
}

void BuildInspector(const InspectorContext& ctx)
{
    const auto* recipe = ctx.World ? ctx.World->GetComponent<SplinePaving>(ctx.Entity) : nullptr;
    if (!recipe || !ctx.Parent)
        return;

    if (const auto* recipes = CurrentRecipes())
    {
        const auto reason = recipes->BuildStates().ReasonOf(*ctx.World, ctx.Entity);
        const auto* status = recipes->PavingStatusOf(ctx.Entity);
        if (!reason.empty())
            InspectorUI::AddInfoCard(ctx.Parent, std::string(reason));
        if (status)
        {
            for (const auto& notice : status->Notices)
                InspectorUI::AddInfoCard(ctx.Parent, notice);
            for (const auto& junction : status->Junctions)
                AddJunctionRow(ctx, junction);
        }
        if (ctx.SimulationRefreshCallbacks && ctx.RequestInspectorRefresh)
        {
            ctx.SimulationRefreshCallbacks->push_back(
                [ctx, shownReason = std::string(reason),
                 shown = status ? std::optional<SplineRecipes::PavingStatus>(*status) : std::nullopt]
                {
                    const auto* current = CurrentRecipes();
                    if (!current)
                        return;
                    const auto* status = current->PavingStatusOf(ctx.Entity);
                    if (shownReason != current->BuildStates().ReasonOf(*ctx.World, ctx.Entity) ||
                        shown.has_value() != (status != nullptr) ||
                        (shown && status && *shown != *status))
                        ctx.RequestInspectorRefresh();
                });
        }
    }

    constexpr std::string_view fields[]{
        "Material", "Seed", "BorderWidth", "BorderHeight", "BorderMaterial", "BorderCorner",
        "Mouth", "MouthRadius", "GroundAuthority", "ConformTarget", "CastShadows", "ReceiveShadows"
    };
    RenderReflectedFieldRows(ctx, ECS::GetComponentTypeId<SplinePaving>(), fields);
    AddMemberRows(ctx, *recipe);
}

} // namespace

void RegisterSplinePavingInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<SplinePaving>(BuildInspector);
    InspectorRegistry::Get().RegisterComponentInspector<Components::SplineExtrude>(BuildExtrudeInspector);
}

} // namespace GameEngine
