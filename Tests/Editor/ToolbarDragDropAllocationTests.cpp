// The allocation contract of ToolbarDragDrop::Setup, measured rather than argued.
//
// SceneViewToolbar::OnPostLayout calls Setup on every layout pass — the wire-once skip gate
// covers WireControls, not this — so Setup is a per-frame path for the life of the editor.
// It used to allocate ~25 times per pass against the scene-view toolbar: five for a
// by-value capture of the caller's container config (one vector block plus four std::strings
// past the 15-char MSVC SSO buffer) and one std::string temporary per button visited, built
// only so HasClass(const std::string&) could hash a 28-char literal.
//
// Behaviour tests cannot see that. A future edit that captures the config by value again, or
// reaches for HasClass(const char*), restores the whole cost while every behavioural arm
// stays green — so the claim needs an arm that fails on allocation count alone.
//
// Its own target on purpose: replacing global operator new is process-wide, and EditorTests
// is a shared binary of 800+ tests. This file follows the same isolation rule the other
// single-purpose targets in this directory are split out for.

#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIElement.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

using GameEngine::Button;
using GameEngine::UIElement;
using GameEngine::Editor::ToolbarDragDrop;

namespace
{
// Armed only around the window under measurement, so gtest's own bookkeeping and the
// fixture's tree construction never enter the count.
std::atomic<bool> g_Armed{false};
std::atomic<size_t> g_Allocations{0};

struct AllocationProbe
{
    AllocationProbe() { g_Allocations.store(0, std::memory_order_relaxed); g_Armed.store(true, std::memory_order_relaxed); }
    ~AllocationProbe() { g_Armed.store(false, std::memory_order_relaxed); }
    static size_t Count() { return g_Allocations.load(std::memory_order_relaxed); }
};
} // namespace

// Replaced as matched pairs. The aligned and nothrow forms are left to the default library
// implementations, which keeps every new/delete pairing internally consistent.
void* operator new(size_t size)
{
    if (g_Armed.load(std::memory_order_relaxed))
        g_Allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size != 0 ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void* operator new[](size_t size)
{
    if (g_Armed.load(std::memory_order_relaxed))
        g_Allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size != 0 ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace
{
const std::vector<ToolbarDragDrop::ContainerConfig>& Containers()
{
    // Verbatim from SceneViewToolbar: the string lengths are the point (23, 26, 19, 33 —
    // every one past the 15-char SSO buffer), so shortening them would void the arm.
    static const std::vector<ToolbarDragDrop::ContainerConfig> kConfig = {
        {"sceneview-toolbar-right", "ui.sceneview.toolbar.order"},
        {"inline-tool-buttons", "ui.sceneview.toolbar.inline.order"},
    };
    return kConfig;
}

// The real scene-view button population: 13 in sceneview-toolbar-right, 7 in
// inline-tool-buttons, matching SceneViewPanel.uxml. The per-button cost was one
// allocation each, so the count is what makes the arm sensitive.
struct ToolbarTree
{
    std::unique_ptr<UIElement> Root = std::make_unique<UIElement>();

    ToolbarTree()
    {
        AddContainer("sceneview-toolbar-right", 13);
        AddContainer("inline-tool-buttons", 7);
    }

  private:
    void AddContainer(const char* cssClass, int buttonCount)
    {
        auto container = std::make_unique<UIElement>();
        container->AddClass(cssClass);
        for (int i = 0; i < buttonCount; ++i)
        {
            auto button = std::make_unique<Button>();
            button->SetId(std::string(cssClass) + "-btn-" + std::to_string(i));
            container->AddChild(std::move(button));
        }
        Root->AddChild(std::move(container));
    }
};
} // namespace

// The headline claim: once the toolbar has settled, the per-frame Setup allocates nothing.
TEST(ToolbarDragDropAllocationTests, SettledSetupPassesAllocateNothing)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;

    // Settle: resolve the containers, mark every button, and size the row vector. The
    // first passes are allowed to allocate; the steady state is what is being pinned.
    for (int pass = 0; pass < 3; ++pass)
        dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    constexpr int kMeasuredPasses = 10;
    size_t allocations = 0;
    {
        AllocationProbe probe;
        for (int pass = 0; pass < kMeasuredPasses; ++pass)
            dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());
        allocations = AllocationProbe::Count();
    }

    EXPECT_EQ(allocations, 0u)
        << "settled ToolbarDragDrop::Setup allocated " << allocations << " times across "
        << kMeasuredPasses << " passes (" << (allocations / kMeasuredPasses)
        << "/pass). A by-value capture of the container config or a HasClass(const char*)"
           " will do this.";
}

// Positive control. A probe that never counts would make the arm above pass no matter what
// Setup does, so prove the instrument can see the allocations it is meant to catch: the
// FIRST Setup over a fresh toolbar must register some.
TEST(ToolbarDragDropAllocationTests, TheProbeSeesTheAllocationsOfAnUnsettledPass)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;

    size_t allocations = 0;
    {
        AllocationProbe probe;
        dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());
        allocations = AllocationProbe::Count();
    }

    EXPECT_GT(allocations, 0u)
        << "the allocation probe counted nothing on a first, unsettled Setup — it is not "
           "wired up, so the zero-allocation arm above proves nothing";
}
