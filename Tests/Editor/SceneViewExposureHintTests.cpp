// The Scene View's exposure notice: when it shows, what it says, how long a measured sentence holds, the
// game camera's sample counting only while it advances, and the notice with Match game camera and Dismiss.
// What Match gives the Scene View is in SceneViewExposureTests.

#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "ECS/ECS.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "EditorContext.h"
#include "InspectorLayoutFixture.h"
#include "SceneView/SceneViewExposureHintOverlay.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/ViewOverlayHost.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::SceneViewExposureHintOverlay;
using Editor::SceneViewExposureMode;
using Editor::SceneViewSettings;
using Editor::ViewOverlayHost;
using Editor::ViewOverlayView;
using InspectorLayoutTesting::FindByClass;
using Comparison = SceneViewExposureHintOverlay::Comparison;
using Measurement = SceneViewExposureHintOverlay::Measurement;
using Message = SceneViewExposureHintOverlay::Message;
using SettingsMatch = SceneViewExposureHintOverlay::SettingsMatch;

namespace
{

// A comparison with a current reading on both sides, the Scene View `sceneMinusGame` stops above the
// game camera in EV100 (a positive value is a darker Scene View).
Comparison Gap(float sceneMinusGame)
{
    Comparison comparison;
    comparison.SceneEv = 12.0f + sceneMinusGame;
    comparison.GameEv = 12.0f;
    return comparison;
}

// What the notice says for one frame with no earlier measurement.
Message DecideOnce(const Comparison& comparison, Message shown)
{
    std::optional<Measurement> last;
    return SceneViewExposureHintOverlay::Decide(comparison, shown, last);
}

// What the notice says, with no current reading, for a Scene View with `view` settings and `camera`.
Message WithoutAReading(const SceneViewSettings::Exposure& view, const Components::Camera& camera)
{
    Comparison comparison;
    comparison.Settings = SceneViewExposureHintOverlay::CompareSettings(view, camera);
    return DecideOnce(comparison, Message::None);
}

// A frame that measures the Scene View 1.6 stops brighter than the game camera, while the settings alone
// count as matched (a stock Scene View and a stock camera).
Comparison BrighterWithStockSettings()
{
    Comparison comparison = Gap(-1.6f);
    comparison.Settings = SettingsMatch::Defaults;
    return comparison;
}

UIElement* HintOn(UIElement& viewport)
{
    return FindByClass(&viewport, "scene-view-exposure-hint");
}

void Click(Button& button)
{
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = &button;
    e.CurrentTarget = &button;
    button.DispatchEvent(e);
}

} // namespace

// The notice shows once the gap passes one stop, and the direction says which way: a Scene View with a
// lower EV100 shows the same light brighter, so the game will look darker than what the user edits.
TEST(SceneViewExposureHint, ShowsPastOneStopInTheGapsDirection)
{
    EXPECT_EQ(DecideOnce(Gap(-SceneViewExposureHintOverlay::kShowAboveStops), Message::None), Message::None)
        << "a gap of exactly the threshold showed the notice";
    EXPECT_EQ(DecideOnce(Gap(-1.05f), Message::None), Message::Brighter);
    EXPECT_EQ(DecideOnce(Gap(1.05f), Message::None), Message::Darker);
    EXPECT_EQ(DecideOnce(Gap(-0.5f), Message::None), Message::None);
}

// Once shown, the notice stays while the gap is at least 0.8 stop and hides below it; hidden, a gap between
// the two does not show it. A gap hovering near one stop neither blinks nor flips.
TEST(SceneViewExposureHint, OnceShownItStaysUntilTheGapFallsBelowPointEight)
{
    EXPECT_FLOAT_EQ(SceneViewExposureHintOverlay::kShowAboveStops, 1.0f);
    EXPECT_FLOAT_EQ(SceneViewExposureHintOverlay::kHideBelowStops, 0.8f);
    EXPECT_EQ(DecideOnce(Gap(-0.9f), Message::Brighter), Message::Brighter);
    EXPECT_EQ(DecideOnce(Gap(-0.8f), Message::Brighter), Message::Brighter);
    EXPECT_EQ(DecideOnce(Gap(-0.79f), Message::Brighter), Message::None);
    EXPECT_EQ(DecideOnce(Gap(0.9f), Message::Darker), Message::Darker);
    EXPECT_EQ(DecideOnce(Gap(0.9f), Message::None), Message::None) << "a hidden notice showed below the threshold";
    EXPECT_EQ(DecideOnce(Gap(-0.9f), Message::SettingsDiffer), Message::None)
        << "a reading that arrives below the threshold kept the settings sentence";

    Message shown = Message::None;
    int changes = 0;
    for (const float gap : {-0.95f, -1.02f, -0.97f, -1.03f, -0.85f, -1.01f, -0.82f})
    {
        const Message next = DecideOnce(Gap(gap), shown);
        changes += next != shown ? 1 : 0;
        shown = next;
    }
    EXPECT_EQ(changes, 1) << "a gap hovering around one stop made the notice blink";
    EXPECT_EQ(shown, Message::Brighter);
}

// Without a current reading on either side, settings that differ show the settings sentence. The same
// settings show nothing, as they do with a reading whatever the gap (that gap comes from the framing).
TEST(SceneViewExposureHint, WithoutAReadingItComparesTheSettings)
{
    Comparison noGameReading;
    noGameReading.SceneEv = 15.0f;
    EXPECT_EQ(DecideOnce(noGameReading, Message::None), Message::SettingsDiffer);
    Comparison noSceneReading;
    noSceneReading.GameEv = 15.0f;
    EXPECT_EQ(DecideOnce(noSceneReading, Message::None), Message::SettingsDiffer);

    noGameReading.Settings = SettingsMatch::Same;
    EXPECT_EQ(DecideOnce(noGameReading, Message::SettingsDiffer), Message::None);
    Comparison framingGap = Gap(-3.0f);
    framingGap.Settings = SettingsMatch::Same;
    EXPECT_EQ(DecideOnce(framingGap, Message::Brighter), Message::None);
}

// A Scene View and a camera at their own defaults count as matched: they differ only in what each ships
// with (the meter and the ceiling), so without a reading no notice shows.
TEST(SceneViewExposureHint, AStockPairCountsAsMatched)
{
    const SceneViewSettings::Exposure stockView;
    const Components::Camera stockCamera{};
    EXPECT_EQ(SceneViewExposureHintOverlay::CompareSettings(stockView, stockCamera), SettingsMatch::Defaults);
    EXPECT_EQ(WithoutAReading(stockView, stockCamera), Message::None) << "the stock pair showed the settings sentence";
}

// A setting changed on one side shows the settings sentence when the other side holds a different value.
// A change to the other side's own default leaves the pair matched.
TEST(SceneViewExposureHint, ASettingChangedOnOneSideShowsTheSettingsSentence)
{
    const SceneViewSettings::Exposure stockView;
    const Components::Camera stockCamera{};
    SceneViewSettings::Exposure brighterView = stockView;
    brighterView.Compensation = 1.0f;
    EXPECT_EQ(WithoutAReading(brighterView, stockCamera), Message::SettingsDiffer);
    SceneViewSettings::Exposure fixedView = stockView;
    fixedView.Mode = SceneViewExposureMode::Fixed;
    EXPECT_EQ(WithoutAReading(fixedView, stockCamera), Message::SettingsDiffer);
    Components::Camera darkerCamera{};
    darkerCamera.ExposureCompensation = -1.0f;
    EXPECT_EQ(WithoutAReading(stockView, darkerCamera), Message::SettingsDiffer);

    Components::Camera lightsCamera{};
    lightsCamera.AutoExposureMeter = Components::ExposureMeter::Lights;
    EXPECT_EQ(WithoutAReading(stockView, lightsCamera), Message::None)
        << "a camera set to the view's own meter showed the settings sentence";
}

// With a setting changed on both sides, different values show the settings sentence and equal values
// count as matched.
TEST(SceneViewExposureHint, SettingsChangedOnBothSidesShowTheSentenceOnlyWhenTheyDiffer)
{
    SceneViewSettings::Exposure view;
    view.Compensation = 1.0f;
    Components::Camera camera{};
    camera.ExposureCompensation = 2.0f;
    EXPECT_EQ(WithoutAReading(view, camera), Message::SettingsDiffer);

    camera.ExposureCompensation = 1.0f;
    EXPECT_EQ(WithoutAReading(view, camera), Message::None)
        << "the same compensation on both sides showed the settings sentence";
}

// A measured sentence holds after the reading goes stale (a hidden Game View stops metering) while nothing
// it was measured in changes. A measured "no notice" holds too.
TEST(SceneViewExposureHint, TheMeasuredSentenceHoldsWhileNothingChanges)
{
    std::optional<Measurement> last;
    Comparison frame = BrighterWithStockSettings();
    ASSERT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::None, last), Message::Brighter);
    frame.GameEv.reset();
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::Brighter, last), Message::Brighter)
        << "the stale reading dropped the measured sentence";
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::Brighter, last), Message::Brighter);

    std::optional<Measurement> quiet;
    Comparison smallGap = Gap(-0.3f);
    ASSERT_EQ(SceneViewExposureHintOverlay::Decide(smallGap, Message::None, quiet), Message::None);
    smallGap.GameEv.reset();
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(smallGap, Message::None, quiet), Message::None)
        << "the stale reading replaced a measured gap below the threshold with the settings sentence";
}

// A change to either side's exposure settings releases the measured sentence, and the settings decide.
TEST(SceneViewExposureHint, ASettingsChangeReleasesTheMeasuredSentence)
{
    std::optional<Measurement> last;
    Comparison frame = BrighterWithStockSettings();
    ASSERT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::None, last), Message::Brighter);
    frame.GameEv.reset();
    frame.Setup.SceneSettings.Compensation = 1.0f;
    frame.Settings = SettingsMatch::Differ;
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::Brighter, last), Message::SettingsDiffer)
        << "a Scene View settings change kept the measured sentence";

    last.reset();
    frame = BrighterWithStockSettings();
    ASSERT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::None, last), Message::Brighter);
    frame.GameEv.reset();
    frame.Setup.GameSettings.Compensation = -1.0f;
    frame.Settings = SettingsMatch::Differ;
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::Brighter, last), Message::SettingsDiffer)
        << "a game camera settings change kept the measured sentence";
}

// Another entity as the active camera releases the measured sentence.
TEST(SceneViewExposureHint, ACameraChangeReleasesTheMeasuredSentence)
{
    std::optional<Measurement> last;
    Comparison frame = BrighterWithStockSettings();
    frame.Setup.GameCamera = ECS::EntityHandle(1, 0);
    ASSERT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::None, last), Message::Brighter);
    frame.GameEv.reset();
    frame.Setup.GameCamera = ECS::EntityHandle(2, 0);
    EXPECT_EQ(SceneViewExposureHintOverlay::Decide(frame, Message::Brighter, last), Message::None)
        << "another camera kept the first camera's measured sentence";
}

// The sentences say that the view is exposed differently and what that means for the game, with no
// number and no photographic unit.
TEST(SceneViewExposureHint, TheSentencesNameTheDirectionWithoutANumber)
{
    EXPECT_EQ(SceneViewExposureHintOverlay::Describe(Message::Brighter),
              "This view is exposed brighter than the game camera, so the game will look darker.");
    EXPECT_EQ(SceneViewExposureHintOverlay::Describe(Message::Darker),
              "This view is exposed darker than the game camera, so the game will look brighter.");
    EXPECT_EQ(SceneViewExposureHintOverlay::Describe(Message::SettingsDiffer),
              "This view's exposure settings differ from the game camera's, so the game may look different.");
    EXPECT_EQ(SceneViewExposureHintOverlay::Describe(Message::None), "");
    for (const Message message : {Message::Brighter, Message::Darker, Message::SettingsDiffer})
    {
        const std::string text = SceneViewExposureHintOverlay::Describe(message);
        EXPECT_EQ(text.find_first_of("0123456789"), std::string::npos) << text;
        EXPECT_EQ(text.find("stop"), std::string::npos) << text;
    }
}

// A metered game-camera sample counts only while its frame keeps advancing: the first sample seen (of
// unknown age, such as a Game View's startup frame before its tab was hidden) does not count, a sample
// that replaced an earlier one counts for one second, and a sample that stops advancing stops counting.
TEST(SceneViewExposureHint, AGameSampleCountsOnlyWhileItKeepsAdvancing)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start{};
    std::optional<SceneViewExposureHintOverlay::GameSample> sample;

    EXPECT_FALSE(SceneViewExposureHintOverlay::ObserveGameSample(sample, 4, start))
        << "the first sample seen counted, though nothing says when it was metered";
    EXPECT_FALSE(SceneViewExposureHintOverlay::ObserveGameSample(sample, 4, start + std::chrono::seconds(30)))
        << "a sample that never advanced counted";

    const Clock::time_point advanced = start + std::chrono::seconds(31);
    EXPECT_TRUE(SceneViewExposureHintOverlay::ObserveGameSample(sample, 5, advanced));
    EXPECT_TRUE(SceneViewExposureHintOverlay::ObserveGameSample(
        sample, 5, advanced + SceneViewExposureHintOverlay::kFreshGameReadingAge));
    EXPECT_FALSE(SceneViewExposureHintOverlay::ObserveGameSample(
        sample, 5, advanced + SceneViewExposureHintOverlay::kFreshGameReadingAge + std::chrono::milliseconds(1)))
        << "a sample that stopped advancing still counted";
    EXPECT_TRUE(SceneViewExposureHintOverlay::ObserveGameSample(sample, 6, advanced + std::chrono::seconds(5)))
        << "a Game View that draws again did not count";
}

// The notice sits on every Scene View and on no Game View, hidden until it has something to say, with its
// Match game camera action and its Dismiss.
TEST(SceneViewExposureHint, TheNoticeIsOnTheSceneViewWithMatchAndDismiss)
{
    EditorContext context;
    ViewOverlayHost host;
    UIElement sceneViewport;
    UIElement gameViewport;
    host.Register(std::make_unique<SceneViewExposureHintOverlay>(context));
    host.PublishViewport(ViewOverlayView::Scene, sceneViewport, {});
    host.PublishViewport(ViewOverlayView::Game, gameViewport, {});
    host.Update();

    EXPECT_EQ(HintOn(gameViewport), nullptr) << "the notice was put on the Game View";
    UIElement* hint = HintOn(sceneViewport);
    ASSERT_NE(hint, nullptr) << "the notice is missing from the Scene View";
    EXPECT_TRUE(hint->HasClass("hidden")) << "the notice shows with no Scene View camera to compare";
    EXPECT_TRUE(hint->HasClass("inspector-notice-opaque"));
    auto* action = dynamic_cast<Button*>(FindByClass(hint, "inspector-notice-action"));
    auto* dismiss = dynamic_cast<Button*>(FindByClass(hint, "inspector-notice-dismiss"));
    ASSERT_NE(action, nullptr) << "InspectorNotice.uxml is staged beside the test executable";
    ASSERT_NE(dismiss, nullptr);
    EXPECT_EQ(action->GetText(), "Match game camera");
    EXPECT_FALSE(action->HasClass("hidden"));
    EXPECT_EQ(dismiss->GetText(), "Dismiss");
    EXPECT_FALSE(dismiss->HasClass("hidden"));
    EXPECT_EQ(dismiss->GetTooltip(), "Hide this notice until the editor restarts");
}

// Dismiss on one Scene View hides the notice on every Scene View at once: the choice is the user's, not
// the view's or the scene's.
TEST(SceneViewExposureHint, DismissHidesTheNoticeOnEverySceneView)
{
    EditorContext context;
    ViewOverlayHost host;
    UIElement perspectiveViewport;
    UIElement topViewport;
    host.Register(std::make_unique<SceneViewExposureHintOverlay>(context));
    host.PublishViewport(ViewOverlayView::Scene, perspectiveViewport, {});
    host.PublishViewport(ViewOverlayView::Scene, topViewport, {});
    UIElement* perspectiveHint = HintOn(perspectiveViewport);
    UIElement* topHint = HintOn(topViewport);
    ASSERT_NE(perspectiveHint, nullptr);
    ASSERT_NE(topHint, nullptr);
    // As the overlay leaves them while they show a sentence.
    for (UIElement* hint : {perspectiveHint, topHint})
        hint->RemoveClass("hidden");
    auto* dismiss = dynamic_cast<Button*>(FindByClass(topHint, "inspector-notice-dismiss"));
    ASSERT_NE(dismiss, nullptr);
    Click(*dismiss);
    EXPECT_TRUE(perspectiveHint->HasClass("hidden")) << "Dismiss on one Scene View left the other's notice up";
    EXPECT_TRUE(topHint->HasClass("hidden")) << "Dismiss left its own notice up";
}
