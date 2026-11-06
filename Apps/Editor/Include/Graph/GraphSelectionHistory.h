#pragma once

#include "UndoRedo/UndoRedoService.h"

#include <string>
#include <unordered_set>

namespace GameEngine::Editor
{
/// Keeps graph history action boundaries aligned with the selected node set.
class GraphSelectionHistory
{
public:
    /// Return false if the pending action cannot be settled before selection changes.
    bool UpdateSelection(const std::unordered_set<std::string>& selectedNodeIds, UndoRedoService* history)
    {
        if (selectedNodeIds == m_SelectedNodeIds)
            return true;
        if (history && !history->CommitPendingEdit())
            return false;
        m_SelectedNodeIds = selectedNodeIds;
        return true;
    }

private:
    std::unordered_set<std::string> m_SelectedNodeIds;
};
} // namespace GameEngine::Editor
