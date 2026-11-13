// The animation panel across a layout reconcile that destroys elements and creates new ones:
// the panel holds no destroyed element, wires each new control once, and keeps the
// controls and views the reconcile did not replace.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Panels/Animation/AnimationWindowPanel.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/IntField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace GameEngine;

namespace
{
const std::filesystem::path kLayoutRelative = std::filesystem::path("UI") / "panels" / "AnimationWindowPanel.uxml";
const std::filesystem::path kStyleRelative = std::filesystem::path("UI") / "panels" / "AnimationWindowPanel.css";

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Replaces the one occurrence of `from` in `text`; an absent or repeated `from` fails the test.
std::string ReplaceOnce(const std::string& text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << "the layout has no \"" << from << "\"";
    EXPECT_EQ(text.find(from, at + 1), std::string::npos) << "the layout has \"" << from << "\" twice";
    if (at == std::string::npos)
        return text;
    std::string out = text;
    out.replace(at, from.size(), to);
    return out;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::seconds budget = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void SendMouse(UIElement& element, EventId id, float x)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &element;
    e.CurrentTarget = &element;
    e.Button = 0;
    e.X = x;
    element.DispatchEvent(e);
}

void Click(UIElement& element)
{
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = &element;
    e.CurrentTarget = &element;
    element.DispatchEvent(e);
}

class AnimationPanelReconcileTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }

        // The panel resolves its layout through the editor source alias. The headless engine
        // mounts none, so the suite mounts a temporary copy of the staged layout it can edit.
        const std::filesystem::path staged = TestPaths::ExecutableDirectory() / "Assets";
        s_Root = TestUtils::MakeUniqueTempDirectory("animation_panel_reconcile");
        std::filesystem::create_directories((s_Root / kLayoutRelative).parent_path());
        s_PristineLayout = ReadText(staged / kLayoutRelative);
        ASSERT_FALSE(s_PristineLayout.empty()) << "the staged AnimationWindowPanel.uxml is missing";
        WriteText(s_Root / kLayoutRelative, s_PristineLayout);
        std::filesystem::copy_file(staged / kStyleRelative, s_Root / kStyleRelative);

        AssetManager& assets = engine.GetAssetManager();
        AssetSourceDesc source{};
        source.Alias = std::string(kAssetSourceAliasEditor);
        source.Root = s_Root;
        ASSERT_TRUE(assets.RegisterSource(source)) << "the editor source alias was already mounted";
        assets.WaitForStartupScan(kAssetSourceAliasEditor);
        s_LayoutGuid = assets.ResolveAssetGuid(kLayoutRelative, kAssetSourceAliasEditor);
        ASSERT_FALSE(s_LayoutGuid.IsNull());
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    // Each test starts from the shipped layout: a layout one test edited stays resident.
    void SetUp() override
    {
        if (ReadText(s_Root / kLayoutRelative) == s_PristineLayout)
            return;
        WriteText(s_Root / kLayoutRelative, s_PristineLayout);
        (void)Assets().ReloadAssetNow(s_LayoutGuid);
    }

    static AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    // One animation panel as the root of its own UIManager, bound to the layout.
    struct MountedPanel
    {
        UIManager Manager{nullptr, &Assets()};
        AnimationWindowPanel* Panel = nullptr;
        UIElement SecondSubtree;

        MountedPanel()
        {
            auto owned = std::make_unique<AnimationWindowPanel>();
            Panel = owned.get();
            Manager.SetRoot(std::move(owned));
            UILayoutAccess::SetLastLayoutRect(*Panel, 0.0f, 0.0f, 1280.0f, 720.0f);
        }

        // Runs the deferred bind and the asynchronous layout load to completion.
        bool Bind()
        {
            Panel->OnPostLayout();
            return WaitUntil(
                [this]
                {
                    Manager.GetDispatcher()->Drain();
                    return Panel->GetToolbarElement() != nullptr && Manager.GetDispatcher()->PendingCount() == 0;
                });
        }

        // Writes `layout` over the bound file, reloads it and binds it into a second subtree,
        // as a second animation panel does. The bind reconciles every subtree bound to the
        // layout, this panel's too, and announces kEventLayoutReconciled to each.
        void Reconcile(const std::string& layout)
        {
            WriteText(s_Root / kLayoutRelative, layout);
            ASSERT_EQ(Assets().ReloadAssetNow(s_LayoutGuid), ReloadOutcome::Reloaded);
            const auto asset = Assets().GetAsset(s_LayoutGuid);
            ASSERT_TRUE(asset && asset->GetType() == AssetType::UILayout);
            ASSERT_TRUE(Manager.BindLayoutToSubtreeChildrenFromAsset(&SecondSubtree,
                                                                     *static_cast<UILayoutAsset*>(asset.get())));
            Manager.GetDispatcher()->Drain();
        }

        // Renames the element `id` away and back: the second reconcile creates a new element
        // with the shipped id, which no earlier pass has seen.
        void ReplaceElement(const std::string& id)
        {
            const std::string attribute = "id=\"" + id + "\"";
            Reconcile(ReplaceOnce(s_PristineLayout, attribute, "id=\"" + id + "Renamed\""));
            Reconcile(s_PristineLayout);
        }

        Button* FindButton(const char* id) const { return dynamic_cast<Button*>(Panel->FindById(HashStringId(id))); }
    };

    static inline std::filesystem::path s_Root;
    static inline std::string s_PristineLayout;
    static inline GUID s_LayoutGuid;
};
} // namespace

// A reconcile that removes the toolbar leaves the panel holding no toolbar.
TEST_F(AnimationPanelReconcileTests, ARemovedElementIsNotHeld)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    UIElement::WeakRef<> toolbar = mounted.Panel->FindById<UIElement>("AnimationWindowToolbar"_sid);
    ASSERT_NE(toolbar.Get(), nullptr);

    mounted.Reconcile(ReplaceOnce(s_PristineLayout, "id=\"AnimationWindowToolbar\"", "id=\"AnimationWindowToolbarRenamed\""));

    ASSERT_EQ(toolbar.Get(), nullptr) << "the reconcile kept the toolbar, so this test proves nothing";
    EXPECT_EQ(mounted.Panel->GetToolbarElement(), nullptr) << "the panel holds the destroyed toolbar";
}

// A button the reconcile creates is wired: one click toggles looping once.
TEST_F(AnimationPanelReconcileTests, AReplacedButtonIsWired)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    UIElement::WeakRef<Button> original = UIElement::MakeWeakRef(mounted.FindButton("AnimationWindowLoop"));
    ASSERT_NE(original.Get(), nullptr);

    mounted.ReplaceElement("AnimationWindowLoop");

    ASSERT_EQ(original.Get(), nullptr) << "the reconcile kept the button, so this test proves nothing";
    Button* replaced = mounted.FindButton("AnimationWindowLoop");
    ASSERT_NE(replaced, nullptr);
    const bool loopBefore = mounted.Panel->GetTimelineState().loop;
    Click(*replaced);
    EXPECT_NE(mounted.Panel->GetTimelineState().loop, loopBefore) << "the replaced Loop button has no click handler";
}

// A button the reconcile keeps is not wired a second time: one click toggles looping once.
TEST_F(AnimationPanelReconcileTests, AKeptButtonFiresOnce)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    Button* loop = mounted.FindButton("AnimationWindowLoop");
    ASSERT_NE(loop, nullptr);
    const uint64 instance = loop->GetInstanceId();

    // One reconcile: a kept button wired again then has two handlers, and a click shows no change.
    mounted.Reconcile(ReplaceOnce(s_PristineLayout, "id=\"AnimationWindowRecord\"", "id=\"AnimationWindowRecordRenamed\""));

    ASSERT_EQ(mounted.FindButton("AnimationWindowLoop"), loop);
    ASSERT_EQ(loop->GetInstanceId(), instance);
    const bool loopBefore = mounted.Panel->GetTimelineState().loop;
    Click(*loop);
    EXPECT_NE(mounted.Panel->GetTimelineState().loop, loopBefore)
        << "one click toggled looping an even number of times: the reconcile wired the kept button again";
}

// A reconcile keeps the toolbar states the user set and the views it did not replace,
// so their selection and scroll stay.
TEST_F(AnimationPanelReconcileTests, AReconcileKeepsTheViewsAndToolbarValues)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    Button* loop = mounted.FindButton("AnimationWindowLoop");
    Button* stacked = mounted.FindButton("AnimationWindowCurvesStacked");
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(stacked, nullptr);
    // Looping on: the layout's Loop button has no "active" class, so a replaced button
    // shows it only when the panel applies the state.
    if (!mounted.Panel->GetTimelineState().loop)
        Click(*loop);
    Click(*stacked);
    const bool looping = mounted.Panel->GetTimelineState().loop;
    ASSERT_TRUE(looping);
    ASSERT_TRUE(loop->HasClass("active"));
    ASSERT_TRUE(stacked->HasClass("active"));
    DopeSheetView* dopeSheet = mounted.Panel->GetDopeSheetView();
    CurvesGraphView* curves = mounted.Panel->GetCurvesGraphView();
    TreeView* properties = mounted.Panel->GetPropertiesTree();
    ASSERT_NE(dopeSheet, nullptr);
    ASSERT_NE(curves, nullptr);
    ASSERT_NE(properties, nullptr);
    const auto viewMode = curves->GetCurveViewMode();

    mounted.ReplaceElement("AnimationWindowLoop");

    Button* loopAfter = mounted.FindButton("AnimationWindowLoop");
    ASSERT_NE(loopAfter, nullptr);
    EXPECT_EQ(mounted.Panel->GetTimelineState().loop, looping);
    EXPECT_EQ(loopAfter->HasClass("active"), looping) << "the replaced Loop button does not show the looping state";
    EXPECT_TRUE(stacked->HasClass("active")) << "the kept Stacked button lost its state";
    EXPECT_EQ(mounted.Panel->GetDopeSheetView(), dopeSheet) << "the reconcile re-created the dope sheet";
    EXPECT_EQ(mounted.Panel->GetCurvesGraphView(), curves) << "the reconcile re-created the curves view";
    EXPECT_EQ(mounted.Panel->GetPropertiesTree(), properties) << "the reconcile re-created the properties tree";
    EXPECT_EQ(curves->GetCurveViewMode(), viewMode) << "the reconcile reset the curves view";
}

// A reconcile that replaces the properties search field, then the panel's destruction:
// the destructor must not read the destroyed field. An AddressSanitizer build reports it.
TEST_F(AnimationPanelReconcileTests, DestroyingThePanelAfterAReconcileReadsNoDestroyedElement)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    UIElement::WeakRef<> search = mounted.Panel->FindById<UIElement>("AnimationWindowPropertiesSearch"_sid);
    ASSERT_NE(search.Get(), nullptr);

    mounted.ReplaceElement("AnimationWindowPropertiesSearch");

    ASSERT_EQ(search.Get(), nullptr) << "the reconcile kept the search field, so this test proves nothing";
    mounted.Manager.SetRoot(nullptr);
}

// A reconcile that replaces a curve option field and keeps its drag label: a drag on the
// label changes the new field and reads no destroyed field.
TEST_F(AnimationPanelReconcileTests, AKeptDragLabelDragsTheReplacedField)
{
    MountedPanel mounted;
    ASSERT_TRUE(mounted.Bind());
    UIElement* label = mounted.Panel->FindById("AnimationWindowNoiseFreqMinLabel"_sid);
    ASSERT_NE(label, nullptr);

    mounted.ReplaceElement("AnimationWindowNoiseFreqMin");

    ASSERT_EQ(mounted.Panel->FindById("AnimationWindowNoiseFreqMinLabel"_sid), label)
        << "the reconcile replaced the label, so this test proves nothing";
    auto* field = dynamic_cast<IntField*>(mounted.Panel->FindById("AnimationWindowNoiseFreqMin"_sid));
    ASSERT_NE(field, nullptr);
    const int before = field->GetValue();
    SendMouse(*label, kEventMouseDown, 0.0f);
    SendMouse(*label, kEventMouseMove, 100.0f);
    EXPECT_NE(field->GetValue(), before) << "the kept label does not drag the replaced field";
}
