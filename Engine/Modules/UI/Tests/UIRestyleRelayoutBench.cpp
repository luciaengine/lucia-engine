// Timing baseline for the element paths a UIElement layout change can move: the
// cascade and relayout, attach and detach with id lookups, and event dispatch
// through a deep ancestor chain.
//
// Every case runs on one generated tree of about 10,000 elements: panels of
// flex rows, each row an icon and two labels with real text, plus one deep
// branch for dispatch. That is one tree shape; a game HUD, an editor inspector
// and a strategy game's unit panel would each be a fair second shape, and every
// header line says which shape ran.
//
// Each case prints its config, sample count, median and p95. Timing numbers are
// only comparable in Release, on the same machine, between two builds of the
// same tree. Each case also checks that it exercised its path (the class
// toggle reached the cascade, every lookup found its element, every handler on
// the chain ran), so a number is never reported for a path that did not run.

#include "IsolatedUIFixture.h"
#include "UIBenchStats.h"

#include "Input/KeyCodes.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using GameEngine::HashStringId;
using GameEngine::StringId;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using GameEngine::UIManager;
using GameEngine::UITesting::BenchClock;
using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::MillisecondsSince;
using GameEngine::UITesting::PrintBenchHeader;
using GameEngine::UITesting::PrintBenchRow;
using GameEngine::UITesting::Summarize;

namespace
{

constexpr const char* kTreeShape = "panels of flex rows (icon + 2 labels), one deep branch";

// 25 panels * 100 rows * (row + icon + 2 labels) = 10,000 elements.
constexpr int kPanels = 25;
constexpr int kRowsPerPanel = 100;
// The detachable subtree: 250 rows * 4 = 1,000 elements, each with an id.
constexpr int kPoolRows = 250;
constexpr int kPoolElements = kPoolRows * 4;
// Ancestors between the dispatch target and the root, each with handlers.
constexpr int kChainDepth = 32;
constexpr int kDispatchesPerSample = 200;
// The events every element of the chain handles, so a dispatch of each walks the whole chain.
constexpr GameEngine::EventId kChainEvents[] = {GameEngine::kEventMouseDown, GameEngine::kEventMouseUp,
                                               GameEngine::kEventKeyDown};
constexpr int kHandlersPerLevel = static_cast<int>(std::size(kChainEvents));
// A click is two pointer events, the press and the release.
constexpr int kPointerEventsPerClick = 2;
constexpr double kMicrosecondsPerMillisecond = 1000.0;

constexpr int kWarmupFrames = 4;
constexpr int kSampleFrames = 40;

// The header details every case prints: the element count of the last frame and the tree's shape.
std::string TreeDetails(const UIManager& ui)
{
    const auto& history = ui.GetUpdateProfilingHistory();
    const uint32_t elements = history.empty() ? 0u : history.back().ElementCount;
    return " | elements=" + std::to_string(elements) + " | tree=" + kTreeShape;
}

void AppendRow(std::string& xml, const std::string& idPrefix, int index)
{
    const std::string number = std::to_string(index);
    if (idPrefix.empty())
    {
        xml += "<uielement class=\"row\"><uielement class=\"icon\"/>";
        xml += "<label class=\"lbl\" text=\"Property Name " + number + "\"/>";
        xml += "<label class=\"val\" text=\"Value " + number + "\"/></uielement>\n";
        return;
    }
    const std::string id = idPrefix + number;
    xml += "<uielement class=\"row\" id=\"" + id + "r\"><uielement class=\"icon\" id=\"" + id + "i\"/>";
    xml += "<label class=\"lbl\" id=\"" + id + "l\" text=\"Pooled Name " + number + "\"/>";
    xml += "<label class=\"val\" id=\"" + id + "v\" text=\"Pooled Value " + number + "\"/></uielement>\n";
}

// The deep branch comes first so its leaf lies inside the viewport the pointer
// is hit-tested against; the panels below it overflow the viewport.
std::string BuildTreeXml()
{
    std::string xml = "<uielement id=\"root\">\n";
    for (int i = 0; i < kChainDepth; ++i)
        xml += "<uielement class=\"chain\" id=\"chain" + std::to_string(i) + "\">";
    xml += "<uielement id=\"leaf\" focusable=\"true\"/>";
    for (int i = 0; i < kChainDepth; ++i)
        xml += "</uielement>";
    xml += "\n<uielement id=\"pool\" class=\"panel\">\n";
    for (int r = 0; r < kPoolRows; ++r)
        AppendRow(xml, "p", r);
    xml += "</uielement>\n";
    for (int p = 0; p < kPanels; ++p)
    {
        xml += "<uielement class=\"panel\">\n";
        for (int r = 0; r < kRowsPerPanel; ++r)
            AppendRow(xml, {}, p * kRowsPerPanel + r);
        xml += "</uielement>\n";
    }
    xml += "</uielement>";
    return xml;
}

// `#root.alt` changes a font size and a row height, so the toggle re-runs the
// cascade on every element and invalidates text measurement and layout. Nothing
// below the root shrinks: the tree overflows the root's 600px, and a shrinking
// column would squeeze every row to its border and the dispatch leaf to zero.
const char* kTreeCss = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
.chain { display: flex; flex-direction: column; flex-shrink: 0; padding: 1px; }
#leaf { flex-shrink: 0; width: 20px; height: 20px; background-color: #4488cc; }
.panel { display: flex; flex-direction: column; flex-shrink: 0; width: 780px; overflow: hidden;
         background-color: #1e1e1e; }
.row { display: flex; flex-direction: row; flex-shrink: 0; align-items: center; height: 24px;
       padding: 2px;
       border-width: 1px; border-color: #333333; background-color: #272727; }
.icon { width: 16px; height: 16px; background-color: #4488cc; border-radius: 3px; }
.lbl { flex-grow: 1; font-family: Roboto; font-size: 12px; color: #dddddd; white-space: nowrap; }
.val { width: 300px; font-family: Roboto; font-size: 12px; color: #99ccff; white-space: nowrap; }
#root.alt .row { height: 26px; }
#root.alt .lbl { font-size: 13px; }
)";

bool BuildTree(IsolatedUIFixture& fx)
{
    if (!fx.Build(1.0f, BuildTreeXml(), kTreeCss))
        return false;
    fx.Manager().SetUpdateProfilingEnabled(true);
    return true;
}

const UIManager::UpdateProfileFrame& LastFrame(const UIManager& ui)
{
    return ui.GetUpdateProfilingHistory().back();
}

void ToggleRootClass(UIElement& root, int frame)
{
    if (frame % 2 == 0)
        root.AddClass("alt");
    else
        root.RemoveClass("alt");
}

// Registers a counting handler for each event on every element from `leaf` up to
// the root, so a dispatch walks the whole chain and runs a handler at each level.
void RegisterChainHandlers(UIElement& leaf, uint64_t& calls)
{
    for (UIElement* el = &leaf; el != nullptr; el = el->GetParent())
    {
        for (GameEngine::EventId id : kChainEvents)
            el->RegisterEventHandler(id, [&calls](UIEvent&) { ++calls; });
    }
}

int ChainLength(const UIElement& leaf)
{
    int length = 0;
    for (const UIElement* el = &leaf; el != nullptr; el = el->GetParent())
        ++length;
    return length;
}

} // namespace

// Case 1: a class toggle on the root, then restyle and relayout of the whole tree
// (the cascade, the rule cache and the Yoga solve).
TEST(UIRestyleRelayoutBench, RootClassToggleRestylesAndRelayouts)
{
    IsolatedUIFixture fx;
    const bool built = BuildTree(fx);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    UIElement* root = fx.Element("root");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(fx.ResolvedFontFamily("p0l"), "Roboto");

    for (int i = 0; i < kWarmupFrames; ++i)
    {
        ToggleRootClass(*root, i);
        fx.StepFrame();
    }
    std::vector<double> wallMs, totalMs, cascadeMs, yogaMs, genMs;
    for (int i = 0; i < kSampleFrames; ++i)
    {
        ToggleRootClass(*root, i);
        const BenchClock::time_point start = BenchClock::now();
        fx.StepFrame();
        wallMs.push_back(MillisecondsSince(start));
        const auto& f = LastFrame(ui);
        totalMs.push_back(f.TotalMs);
        cascadeMs.push_back(f.CascadeComputeMs);
        yogaMs.push_back(f.YogaMs);
        genMs.push_back(f.GenAllPrimitivesMs);
    }

    PrintBenchHeader("Case 1: root class toggle, restyle + relayout (ms per frame)", TreeDetails(ui));
    PrintBenchRow("wall Update+Render", Summarize(wallMs));
    PrintBenchRow("TotalMs (profiler)", Summarize(totalMs));
    PrintBenchRow("CascadeComputeMs", Summarize(cascadeMs));
    PrintBenchRow("YogaMs", Summarize(yogaMs));
    PrintBenchRow("GenAllPrimitivesMs", Summarize(genMs));

    // The toggle has to reach layout, or the frames above timed nothing. The
    // samples end with the class removed, so adding it must grow the row.
    ASSERT_FALSE(root->HasClass("alt"));
    const float rowHeight = fx.Element("p0r")->GetLayoutHeight();
    root->AddClass("alt");
    fx.StepFrame();
    EXPECT_GT(fx.Element("p0r")->GetLayoutHeight(), rowHeight);
}

// Case 2: attach and detach a 1,000-element subtree, and look up every element
// in it by id while attached (the attach settle and the id index).
TEST(UIRestyleRelayoutBench, SubtreeAttachDetachWithIdLookups)
{
    IsolatedUIFixture fx;
    const bool built = BuildTree(fx);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    UIElement* root = fx.Element("root");
    UIElement* poolElement = fx.Element("pool");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(poolElement, nullptr);

    std::vector<StringId> ids;
    ids.reserve(kPoolElements);
    for (int r = 0; r < kPoolRows; ++r)
    {
        const std::string id = "p" + std::to_string(r);
        for (const char* suffix : {"r", "i", "l", "v"})
            ids.push_back(HashStringId(id + suffix));
    }

    std::unique_ptr<UIElement> pool = root->TakeChild(poolElement);
    ASSERT_NE(pool, nullptr);
    fx.StepFrame();

    std::vector<double> attachMs, lookupMs, detachMs;
    int missedLookups = 0;
    for (int i = 0; i < kWarmupFrames + kSampleFrames; ++i)
    {
        BenchClock::time_point start = BenchClock::now();
        root->AddChild(std::move(pool));
        fx.StepFrame();
        const double attach = MillisecondsSince(start);

        start = BenchClock::now();
        int found = 0;
        for (StringId id : ids)
            found += root->FindById(id) != nullptr ? 1 : 0;
        const double lookup = MillisecondsSince(start);
        missedLookups += kPoolElements - found;

        start = BenchClock::now();
        pool = root->TakeChild(poolElement);
        fx.StepFrame();
        const double detach = MillisecondsSince(start);
        ASSERT_NE(pool, nullptr);

        if (i < kWarmupFrames)
            continue;
        attachMs.push_back(attach);
        lookupMs.push_back(lookup);
        detachMs.push_back(detach);
    }

    PrintBenchHeader("Case 2: attach + detach a 1,000-element subtree (ms)", TreeDetails(ui));
    PrintBenchRow("AddChild + frame", Summarize(attachMs));
    PrintBenchRow("1,000 FindById lookups", Summarize(lookupMs));
    PrintBenchRow("TakeChild + frame", Summarize(detachMs));

    EXPECT_EQ(missedLookups, 0) << "a lookup missed, so the id index was not what was timed";
    EXPECT_EQ(root->FindById(ids.front()), nullptr) << "the detached subtree is still findable from the root";
}

// Case 3: pointer and key events dispatched to the leaf of a deep chain, with a
// handler for each event at every level (the dispatch walk).
TEST(UIRestyleRelayoutBench, DeepChainPointerAndKeyDispatch)
{
    IsolatedUIFixture fx;
    const bool built = BuildTree(fx);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    UIElement* leaf = fx.Element("leaf");
    ASSERT_NE(leaf, nullptr);

    uint64_t calls = 0;
    RegisterChainHandlers(*leaf, calls);
    const int chainLength = ChainLength(*leaf);

    // OnMouseMove takes logical px; the press targets the element under the
    // pointer on the last completed layout, so one frame settles it.
    ui.OnMouseMove(leaf->GetLayoutX() + leaf->GetLayoutWidth() * 0.5f,
                   leaf->GetLayoutY() + leaf->GetLayoutHeight() * 0.5f);
    fx.StepFrame();
    ui.SetFocusById("leaf");
    fx.StepFrame();

    constexpr int kKeyActionPress = 1;
    std::vector<double> pointerUs, keyUs;
    uint64_t expectedCalls = 0;
    calls = 0;
    for (int i = 0; i < kWarmupFrames + kSampleFrames; ++i)
    {
        BenchClock::time_point start = BenchClock::now();
        for (int d = 0; d < kDispatchesPerSample; ++d)
        {
            ui.OnMouseButton(0, true);
            ui.OnMouseButton(0, false);
        }
        const double pointer = MillisecondsSince(start) * kMicrosecondsPerMillisecond /
                               (kPointerEventsPerClick * kDispatchesPerSample);

        start = BenchClock::now();
        for (int d = 0; d < kDispatchesPerSample; ++d)
            ui.OnKey(GameEngine::Input::kKeyCode_A, kKeyActionPress, 0);
        const double key = MillisecondsSince(start) * kMicrosecondsPerMillisecond / kDispatchesPerSample;

        fx.StepFrame();
        expectedCalls += static_cast<uint64_t>(kHandlersPerLevel * kDispatchesPerSample * chainLength);
        if (i < kWarmupFrames)
            continue;
        pointerUs.push_back(pointer);
        keyUs.push_back(key);
    }

    PrintBenchHeader("Case 3: dispatch through a deep chain (us per event)", TreeDetails(ui));
    std::printf("  chain length (leaf to root) = %d, handlers per level = %d\n", chainLength, kHandlersPerLevel);
    PrintBenchRow("MouseDown or MouseUp", Summarize(pointerUs));
    PrintBenchRow("KeyDown", Summarize(keyUs));

    EXPECT_EQ(calls, expectedCalls) << "not every handler on the chain ran, so the walk timed is "
                                       "not the whole chain";
}
