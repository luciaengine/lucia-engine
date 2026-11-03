#include "Editor/Entities/ComponentRemoval.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
void NotifyComponentChanged(EditorChangeNotifications* notifications, ECS::World* world, ECS::EntityHandle entity,
                            ECS::ComponentTypeId typeId)
{
    if (!notifications)
        return;
    notifications->NotifyComponentChanged({world, entity, typeId, EditorChangeNotifications::ChangeKind::Commit});
}

class RemoveComponentCommand final : public IEditorCommand
{
  public:
    RemoveComponentCommand(std::string name,
                           ECS::World* world,
                           EditorChangeNotifications* notifications,
                           ECS::EntityHandle entity,
                           ECS::ComponentTypeId typeId)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_TypeId(typeId)
    {
        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
        {
            EditorComponentTraits traits;
            m_RemoveStackBundle = EditorComponentTraitsRegistry::Get().TryGet(m_TypeId, traits) &&
                                  traits.RemovingRemovesStackEffects && traits.HostsInspectorSection;

            if (m_RemoveStackBundle)
            {
                // Volume snapshot FIRST — Redo() removes effects before the
                // Volume via m_Snapshots.front(), Undo() restores in capture
                // order. The effects are every component on the entity whose
                // section the volume hosts.
                CaptureSnapshot(m_TypeId);
                if (ECS::Archetype* archetype = m_World->GetEntityArchetype(m_Entity))
                {
                    for (ECS::ComponentTypeId componentTypeId : archetype->GetSignature().GetComponents())
                    {
                        if (traits.HostsInspectorSection(componentTypeId))
                            CaptureSnapshot(componentTypeId);
                    }
                }

                for (const auto& snap : m_Snapshots)
                {
                    if (snap.HadComponent)
                    {
                        m_HadComponent = true;
                        break;
                    }
                }
            }
            else
            {
                m_HadComponent = m_World->CaptureComponentBytes(m_Entity, m_TypeId, m_SnapshotBytes);
            }
        }
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (!m_HadComponent)
            return;

        if (m_RemoveStackBundle)
        {
            for (const auto& snap : m_Snapshots)
            {
                if (!snap.HadComponent)
                    continue;
                if (m_World->ApplyComponentBytesImmediate(m_Entity, snap.TypeId, snap.Bytes))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, snap.TypeId);
            }
            return;
        }

        if (m_World->ApplyComponentBytesImmediate(m_Entity, m_TypeId, m_SnapshotBytes))
        {
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (!m_HadComponent)
            return;

        if (m_RemoveStackBundle)
        {
            for (size_t i = 1; i < m_Snapshots.size(); ++i)
            {
                const auto& snap = m_Snapshots[i];
                if (!snap.HadComponent)
                    continue;
                if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, snap.TypeId))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, snap.TypeId);
            }
            const auto& volumeSnap = m_Snapshots.front();
            if (volumeSnap.HadComponent &&
                m_World->RemoveComponentByTypeIdImmediate(m_Entity, volumeSnap.TypeId))
            {
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, volumeSnap.TypeId);
            }
            return;
        }

        if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_TypeId))
        {
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
    }

  private:
    struct ComponentSnapshot
    {
        ECS::ComponentTypeId TypeId{};
        bool HadComponent = false;
        std::vector<uint8_t> Bytes;
    };

    void CaptureSnapshot(ECS::ComponentTypeId typeId)
    {
        ComponentSnapshot snapshot{};
        snapshot.TypeId = typeId;
        snapshot.HadComponent = m_World->CaptureComponentBytes(m_Entity, typeId, snapshot.Bytes);
        m_Snapshots.push_back(std::move(snapshot));
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;                                // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_TypeId{};
    bool m_HadComponent = false;
    // Removing a volume that takes its stack effects with it.
    bool m_RemoveStackBundle = false;
    std::vector<uint8_t> m_SnapshotBytes;
    std::vector<ComponentSnapshot> m_Snapshots;
};
} // namespace

void CommitComponentRemoval(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                            ECS::EntityHandle entity, ECS::ComponentTypeId typeId, const std::string& label)
{
    if (!undo)
    {
        RemoveComponentCommand(label, &world, notifications, entity, typeId).Redo();
        return;
    }
    CommitGenericEdit(world, *undo, label, [&]() {
        undo->Execute(std::make_unique<RemoveComponentCommand>(label, &world, notifications, entity, typeId));
    });
}

} // namespace GameEngine::Editor
