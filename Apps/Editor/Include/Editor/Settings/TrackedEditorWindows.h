#pragma once

#include <memory>
#include <vector>

namespace GameEngine
{
class UIManager;
struct Stylesheet;

namespace Editor
{

// The editor windows an app-wide appearance setting has been applied to, so a
// later change reaches every one of them. A setting owns one instance: it adds
// each window once after the window's theme is attached, and on a change
// visits the windows that are still alive.
class TrackedEditorWindows
{
public:
    // Start tracking `ui`; a window already tracked is left as it is.
    void Add(UIManager* ui);

    // Drop windows whose UIManager has been destroyed, then call `fn` with
    // each remaining one.
    template <class Fn>
    void ForEachLive(Fn&& fn)
    {
        DropDead();
        for (Entry& entry : m_Entries)
            fn(*entry.Ui);
    }

    // Replace the stylesheet this owner keeps in `ui` with `sheet` (nullptr
    // removes it), at the cascade position the previous one held so the
    // override stays after the theme. `ui` must have been added.
    void SetOwnedStylesheet(UIManager& ui, std::shared_ptr<const Stylesheet> sheet);

    // Add or remove `className` on the root of `ui` and restyle it.
    static void SetRootClass(UIManager& ui, const char* className, bool enabled);

private:
    struct Entry
    {
        UIManager* Ui = nullptr;
        std::shared_ptr<const Stylesheet> Sheet;
    };

    void DropDead();

    std::vector<Entry> m_Entries;
};

} // namespace Editor
} // namespace GameEngine
