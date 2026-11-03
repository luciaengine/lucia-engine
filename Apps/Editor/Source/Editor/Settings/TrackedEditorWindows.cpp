#include "Editor/Settings/TrackedEditorWindows.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Editor
{

void TrackedEditorWindows::Add(UIManager* ui)
{
    if (!ui)
        return;
    const bool tracked = std::any_of(m_Entries.begin(), m_Entries.end(),
                                     [ui](const Entry& entry) { return entry.Ui == ui; });
    if (!tracked)
        m_Entries.push_back(Entry{ui, nullptr});
}

void TrackedEditorWindows::SetOwnedStylesheet(UIManager& ui, std::shared_ptr<const Stylesheet> sheet)
{
    auto it = std::find_if(m_Entries.begin(), m_Entries.end(),
                           [&ui](const Entry& entry) { return entry.Ui == &ui; });
    assert(it != m_Entries.end() && "SetOwnedStylesheet on a window that was never added");
    if (it == m_Entries.end())
        return;

    std::vector<const Stylesheet*> oldBlock;
    if (it->Sheet)
        oldBlock.push_back(it->Sheet.get());
    std::vector<StylesheetHandle> newBlock;
    if (sheet)
        newBlock.push_back(sheet);

    ui.ReplaceGlobalStylesheetBlock(oldBlock, newBlock);
    it->Sheet = std::move(sheet);
    ui.MarkStyleDirtyAll();
}

void TrackedEditorWindows::SetRootClass(UIManager& ui, const char* className, bool enabled)
{
    UIElement* root = ui.GetRootElement();
    if (!root)
        return;
    if (enabled)
        root->AddClass(className);
    else
        root->RemoveClass(className);
    ui.MarkStyleDirtyAll();
}

void TrackedEditorWindows::DropDead()
{
    std::erase_if(m_Entries, [](const Entry& entry) { return !UIManager::IsAlive(entry.Ui); });
}

} // namespace GameEngine::Editor
