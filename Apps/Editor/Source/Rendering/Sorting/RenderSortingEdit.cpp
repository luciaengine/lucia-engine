#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/RenderSorting.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
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
const std::string kOrderEditName = "Change Render Order";

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

// One snapshot for a selection: per entity, a presence byte, then the component bytes when present.
bool CaptureSortingSet(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
                       UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    bytes.clear();
    for (const auto entity : entities)
    {
        if (!world.IsValid(entity))
            return false;
        const auto* value = world.GetComponent<Sorting>(entity);
        bytes.push_back(value ? 1u : 0u);
        if (!value)
            continue;
        const size_t at = bytes.size();
        bytes.resize(at + sizeof(Sorting));
        std::memcpy(bytes.data() + at, value, sizeof(Sorting));
    }
    return true;
}

bool RestoreSortingSet(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
                       const UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    size_t at = 0;
    for (const auto entity : entities)
    {
        if (!world.IsValid(entity) || at >= bytes.size())
            return false;
        if (bytes[at++] == 0u)
        {
            world.RemoveComponentImmediate<Sorting>(entity);
            continue;
        }
        if (at + sizeof(Sorting) > bytes.size())
            return false;
        Sorting value{};
        std::memcpy(&value, bytes.data() + at, sizeof(Sorting));
        at += sizeof(Sorting);
        world.AddComponentImmediate(entity, value);
    }
    return at == bytes.size();
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
        if (m_Undo)
            m_Edit.Preview([this, world, order]() { WriteOrders(*world, order); });
        else
        {
            WriteOrders(*world, order);
            if (m_Notifications)
                for (const auto entity : m_Targets)
                    m_Notifications->NotifyComponentChange<Sorting>(world, entity,
                        commit ? EditorChangeNotifications::ChangeKind::Commit :
                                 EditorChangeNotifications::ChangeKind::Preview);
        }
        if (!commit)
            return;
        if (m_Undo)
            m_Edit.Commit();
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
        }
        if (m_Undo)
            m_Edit = m_Undo->BeginInteractiveEdit(kOrderEditName, MakeSelectionTarget());
        m_Started = true;
        return true;
    }

    // One undo target for the whole selection. The edit may add the component, so the
    // target asks for live entities, not for the component.
    UndoRedoService::SnapshotTarget MakeSelectionTarget() const
    {
        auto target = MultiEntityUndo::MakeComponentSnapshotTarget(
            m_ResolveWorld, m_Targets, ECS::GetComponentTypeId<Sorting>(), m_Notifications, kOrderEditName);
        target.Validate = [resolve = m_ResolveWorld, entities = m_Targets]()
        {
            auto* world = resolve();
            return world && std::all_of(entities.begin(), entities.end(),
                                        [world](ECS::EntityHandle entity) { return world->IsValid(entity); });
        };
        target.Capture = [resolve = m_ResolveWorld, entities = m_Targets](auto& bytes)
        {
            auto* world = resolve();
            return world && CaptureSortingSet(*world, entities, bytes);
        };
        target.Apply = [resolve = m_ResolveWorld, entities = m_Targets](const auto& bytes)
        {
            auto* world = resolve();
            return world && RestoreSortingSet(*world, entities, bytes);
        };
        target.Describe = [](const auto&, const auto&) { return kOrderEditName; };
        return target;
    }

    void WriteOrders(ECS::World& world, int32 order)
    {
        for (size_t index = 0; index < m_Targets.size(); ++index)
            if (world.IsValid(m_Targets[index]))
                WriteOrder(world, index, order);
    }

    void WriteOrder(ECS::World& world, size_t index, int32 order)
    {
        const auto entity = m_Targets[index];
        const auto* current = world.GetComponent<Sorting>(entity);
        Sorting updated = current ? *current : Sorting{};
        updated.OrderInLayer = order;
        if (!m_HadSorting[index] && updated.Layer == 0 && order == 0 && !updated.GroupChildren &&
            !updated.SortByY && updated.SortOriginY == 0.0f)
            world.RemoveComponentImmediate<Sorting>(entity);
        else
            world.AddComponentImmediate(entity, updated);
    }

    void Reset()
    {
        m_Edit = {};
        m_Targets.clear();
        m_HadSorting.clear();
        m_Started = false;
    }

    std::function<ECS::World*()> m_ResolveWorld;
    std::vector<ECS::EntityHandle> m_Entities;
    std::vector<ECS::EntityHandle> m_Targets;
    std::vector<bool> m_HadSorting;
    UndoRedoService::InteractiveEdit m_Edit;
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
            target.Validate = [&world, entity]() { return world.IsValid(entity); };
            target.Capture = [&world, entity](auto& bytes) { return CaptureSorting(world, entity, bytes); };
            target.Apply = [&world, entity](const auto& bytes) { return RestoreSorting(world, entity, bytes); };
            target.Describe = [](const auto&, const auto&) { return kEditName; };
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
