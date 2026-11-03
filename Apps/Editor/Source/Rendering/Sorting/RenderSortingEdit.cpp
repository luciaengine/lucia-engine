#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/RenderSorting.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;
std::vector<ECS::ComponentTypeId> g_RendererComponents;
const std::string kEditName = "Change Sorting Layer";

bool CaptureSorting(ECS::World& world, ECS::EntityHandle entity,
                    UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    if (!world.IsValid(entity))
        return false;
    if (const auto* value = world.GetComponent<Sorting>(entity))
    {
        bytes.resize(sizeof(Sorting));
        std::memcpy(bytes.data(), value, sizeof(Sorting));
    }
    else
        bytes.clear();
    return true;
}

bool RestoreSorting(ECS::World& world, ECS::EntityHandle entity,
                    const UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    if (!world.IsValid(entity))
        return false;
    if (bytes.empty())
    {
        world.RemoveComponentImmediate<Sorting>(entity);
        return true;
    }
    if (bytes.size() != sizeof(Sorting))
        return false;
    Sorting value{};
    std::memcpy(&value, bytes.data(), sizeof(Sorting));
    world.AddComponentImmediate(entity, value);
    return true;
}

class SortingOrderEdit
{
public:
    explicit SortingOrderEdit(const InspectorContext& context)
        : m_Undo(context.Undo), m_Notifications(context.ChangeNotifications)
    {
        m_ResolveWorld = [expected = context.World, getWorld = context.GetWorld]() -> ECS::World*
        {
            return !getWorld || getWorld() == expected ? expected : nullptr;
        };
        m_Entities.push_back(context.Entity);
        for (const auto entity : InspectorDrag::GetAdditionalEntities(context))
            m_Entities.push_back(entity);
    }

    void Apply(int32 order, bool commit)
    {
        auto* world = m_ResolveWorld();
        if (!world || !world->IsValid(m_Entities.front()))
        {
            Reset();
            return;
        }
        if (!m_Started && !Begin(*world, order))
            return;
        for (size_t index = 0; index < m_Targets.size(); ++index)
        {
            const auto entity = m_Targets[index];
            if (!world->IsValid(entity))
                continue;
            if (m_Undo)
                m_Edits[index].Preview([this, world, index, order]() { WriteOrder(*world, index, order); });
            else
            {
                WriteOrder(*world, index, order);
                if (m_Notifications)
                    m_Notifications->NotifyComponentChange<Sorting>(world, entity,
                        commit ? EditorChangeNotifications::ChangeKind::Commit :
                                 EditorChangeNotifications::ChangeKind::Preview);
            }
        }
        if (!commit)
            return;
        if (m_Undo)
        {
            m_Undo->BeginCompound("Change Render Order");
            for (auto& edit : m_Edits)
                edit.Commit();
            m_Undo->EndCompound();
        }
        Reset();
    }

private:
    bool Begin(ECS::World& world, int32 order)
    {
        bool changed = false;
        for (const auto entity : m_Entities)
        {
            if (!SupportsRenderSorting(world, entity))
                continue;
            const auto* current = world.GetComponent<Sorting>(entity);
            changed |= (current ? current->OrderInLayer : 0) != order;
        }
        if (!changed)
            return false;
        for (const auto entity : m_Entities)
        {
            if (!SupportsRenderSorting(world, entity))
                continue;
            m_Targets.push_back(entity);
            m_HadSorting.push_back(world.HasComponent<Sorting>(entity));
            if (!m_Undo)
                continue;
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Sorting>(
                m_ResolveWorld, entity, m_Notifications, "Change Render Order");
            target.Capture = [resolve = m_ResolveWorld, entity](auto& bytes)
            {
                auto* activeWorld = resolve();
                return activeWorld && CaptureSorting(*activeWorld, entity, bytes);
            };
            target.Apply = [resolve = m_ResolveWorld, entity](const auto& bytes)
            {
                auto* activeWorld = resolve();
                return activeWorld && RestoreSorting(*activeWorld, entity, bytes);
            };
            m_Edits.push_back(m_Undo->BeginInteractiveEdit("Change Render Order", std::move(target)));
        }
        m_Started = true;
        return true;
    }

    void WriteOrder(ECS::World& world, size_t index, int32 order)
    {
        const auto entity = m_Targets[index];
        const auto* current = world.GetComponent<Sorting>(entity);
        Sorting updated = current ? *current : Sorting{};
        updated.OrderInLayer = order;
        if (!m_HadSorting[index] && updated.Layer == 0 && order == 0 && !updated.GroupChildren)
            world.RemoveComponentImmediate<Sorting>(entity);
        else
            world.AddComponentImmediate(entity, updated);
    }

    void Reset()
    {
        m_Edits.clear();
        m_Targets.clear();
        m_HadSorting.clear();
        m_Started = false;
    }

    std::function<ECS::World*()> m_ResolveWorld;
    std::vector<ECS::EntityHandle> m_Entities;
    std::vector<ECS::EntityHandle> m_Targets;
    std::vector<bool> m_HadSorting;
    std::vector<UndoRedoService::InteractiveEdit> m_Edits;
    UndoRedoService* m_Undo;
    EditorChangeNotifications* m_Notifications;
    bool m_Started = false;
};

}

bool SupportsRenderSorting(ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.IsValid(entity))
        return false;
    return world.HasComponent<Sorting>(entity) || HasSortingRenderer(world, entity);
}

bool HasSortingRenderer(ECS::World& world, ECS::EntityHandle entity)
{
    return world.IsValid(entity) && std::any_of(g_RendererComponents.begin(), g_RendererComponents.end(),
        [&world, entity](ECS::ComponentTypeId type) { return world.HasComponent(entity, type); });
}

void ApplyRenderSortingLayer(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
    int32 layer, UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    if (undo)
        undo->BeginCompound(kEditName);
    for (const auto entity : entities)
    {
        if (!SupportsRenderSorting(world, entity))
            continue;
        const auto* current = world.GetComponent<Sorting>(entity);
        if ((current ? current->Layer : 0) == layer)
            continue;
        Sorting updated = current ? *current : Sorting{};
        updated.Layer = layer;
        if (undo)
        {
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Sorting>(&world, entity, notifications, kEditName);
            target.Capture = [&world, entity](auto& bytes) { return CaptureSorting(world, entity, bytes); };
            target.Apply = [&world, entity](const auto& bytes) { return RestoreSorting(world, entity, bytes); };
            auto edit = undo->BeginInteractiveEdit(kEditName, std::move(target));
            world.AddComponentImmediate(entity, updated);
            edit.Commit();
        }
        else
            CommitComponentUpdate(&world, entity, notifications, updated);
    }
    if (undo)
        undo->EndCompound();
}

std::pair<std::function<void(int)>, std::function<void(int)>>
MakeRenderSortingOrderHandlers(const InspectorContext& context)
{
    auto edit = std::make_shared<SortingOrderEdit>(context);
    return {[edit](int value) { edit->Apply(value, false); },
            [edit](int value) { edit->Apply(value, true); }};
}

void RegisterRenderSortingComponent(ECS::ComponentTypeId component)
{
    if (std::find(g_RendererComponents.begin(), g_RendererComponents.end(), component) == g_RendererComponents.end())
        g_RendererComponents.push_back(component);
}

}
