#include "Prefabs/PrefabOverridesList/DiscardStaleOverrideCommand.h"

#include "Scene/SceneDocumentManager.h"

#include <utility>

namespace GameEngine::Editor
{

DiscardStaleOverrideCommand::DiscardStaleOverrideCommand(SceneDocumentManager& document, ECS::World& world,
                                                         EditorChangeNotifications* notifications,
                                                         ECS::EntityHandle root, std::size_t skipIndex)
    : m_Document(document)
    , m_World(world)
    , m_Notifications(notifications)
    , m_Root(root)
    , m_SkipIndex(skipIndex)
{
}

void DiscardStaleOverrideCommand::Do()
{
    m_Skip = m_Document.DiscardLoadSkip(m_SkipIndex);
    NotifyRoot(EditorChangeNotifications::ChangeKind::Commit);
}

void DiscardStaleOverrideCommand::Undo()
{
    if (!m_Skip)
        return;
    m_Document.RestoreLoadSkip(m_SkipIndex, std::move(*m_Skip));
    m_Skip.reset();
    NotifyRoot(EditorChangeNotifications::ChangeKind::UndoRedo);
}

void DiscardStaleOverrideCommand::NotifyRoot(EditorChangeNotifications::ChangeKind kind) const
{
    if (m_Notifications)
        m_Notifications->NotifyComponentChanged({&m_World, m_Root, 0, kind});
}

} // namespace GameEngine::Editor
