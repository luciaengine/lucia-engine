// A menu built before a backend change picks the change up at its next Clear(),
// where InterceptableContextMenu swaps its inner implementation. Handlers set
// on the old inner do not travel with the swap, so the decorator re-applies
// them — the close handler included: the graph canvas retires its pending wire
// from that callback, and a dropped one leaves the wire painted forever.

#include <gtest/gtest.h>

#include "EditorContextMenu/ContextMenuBackendPolicy.h"
#include "EditorContextMenu/InterceptableContextMenu.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>

using namespace GameEngine;

namespace
{

// The registry keys on the pointer alone; Show() resolves the UIManager from it
// and never dereferences the window.
Platform::Window* FakeWindow()
{
    return reinterpret_cast<Platform::Window*>(0x1);
}

class BackendGuard
{
  public:
    BackendGuard() : m_Restore(GetContextMenuBackend()) {}
    ~BackendGuard() { SetContextMenuBackend(m_Restore); }
    BackendGuard(const BackendGuard&) = delete;
    BackendGuard& operator=(const BackendGuard&) = delete;

  private:
    ContextMenuBackend m_Restore;
};

} // namespace

TEST(ContextMenuBackendSwap, TheCloseHandlerSurvivesTheInnerBackendSwap)
{
    BackendGuard restoreBackend;
    UIManager manager{nullptr};
    // Show() parents its overlay under the root, so a manager without one never
    // opens and never has a dismissal to report.
    manager.SetRoot(std::make_unique<UIElement>());
    UIContextMenu::Register(FakeWindow(), &manager);

    SetContextMenuBackend(ContextMenuBackend::BuiltIn);
    // Declared before the menu: destroying an open menu reports its dismissal,
    // so the handler runs from the menu's destructor at the end of this scope.
    bool closed = false;
    auto menu = CreateContextMenu();
    ASSERT_NE(menu, nullptr);

    menu->SetCloseHandler([&closed]() { closed = true; });

    // Two changes so the generation moves on every platform and the selection
    // still lands on the built-in menu (the only backend a test can drive).
    SetContextMenuBackend(ContextMenuBackend::Native);
    SetContextMenuBackend(ContextMenuBackend::BuiltIn);

    menu->Clear();
    menu->AddItem(0, "Item", 1);
    menu->Show(FakeWindow(), 10, 10);
    ASSERT_FALSE(closed) << "showing a menu must not report it closed";

    // Opening the next menu tears the first one down, which is a dismissal.
    menu->Clear();
    menu->AddItem(0, "Item", 1);
    menu->Show(FakeWindow(), 20, 20);
    EXPECT_TRUE(closed) << "the swapped-in backend never got the close handler";

    UIContextMenu::Unregister(FakeWindow());
}

// A menu that another menu displaces stays open, parked, until the next drain
// releases it; its destructor is the only place its dismissal is reported.
TEST(ContextMenuBackendSwap, ADisplacedMenuReportsItsCloseWhenReleased)
{
    BackendGuard restoreBackend;
    UIManager manager{nullptr};
    manager.SetRoot(std::make_unique<UIElement>());
    UIContextMenu::Register(FakeWindow(), &manager);
    SetContextMenuBackend(ContextMenuBackend::BuiltIn);

    bool firstClosed = false;
    {
        auto first = CreateContextMenu();
        ASSERT_NE(first, nullptr);
        first->SetCloseHandler([&firstClosed]() { firstClosed = true; });
        first->AddItem(0, "First", 1);
        first->Show(FakeWindow(), 10, 10);
    }
    ASSERT_FALSE(firstClosed) << "the parked menu outlives its caller's wrapper";

    auto second = CreateContextMenu();
    ASSERT_NE(second, nullptr);
    second->AddItem(0, "Second", 1);
    second->Show(FakeWindow(), 20, 20);
    ASSERT_FALSE(firstClosed) << "the displaced menu is parked until the drain";
    DrainShowingContextMenus();
    EXPECT_TRUE(firstClosed) << "the displaced menu was released without reporting its close";

    UIContextMenu::Unregister(FakeWindow());
}
