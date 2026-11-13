#pragma once

#include "ECS/ECS.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
struct EditorContext;
namespace Components
{
struct Camera;
} // namespace Components
namespace EditorUI
{
class InspectorNotice;
} // namespace EditorUI
} // namespace GameEngine

namespace GameEngine::Editor
{

/// A notice along the top of the Scene View when its exposure differs from the game camera's, with
/// "Match game camera" (for this session) and "Dismiss" (until the editor restarts). Placed by
/// UI/controls/ViewOverlay/SceneViewExposureHint.css.
class SceneViewExposureHintOverlay final : public ViewOverlay
{
  public:
    /// The notice shows when the two exposures are more than this many stops apart.
    static constexpr float kShowAboveStops = 1.0f;
    /// A shown notice stays until the gap falls below this, so a gap near the threshold does not blink.
    static constexpr float kHideBelowStops = 0.8f;
    /// A metered game-camera sample is a current reading while its frame advanced within this time. A
    /// hidden Game View does not draw, so its sample stops.
    static constexpr std::chrono::milliseconds kFreshGameReadingAge{1000};

    /// What the notice says.
    enum class Message : uint8_t
    {
        None,
        /// The Scene View shows the world brighter than the game camera does.
        Brighter,
        /// The Scene View shows the world darker than the game camera does.
        Darker,
        /// The exposure settings differ, and no current reading measures the gap.
        SettingsDiffer,
    };

    /// How the Scene View's exposure settings compare with the game camera's.
    enum class SettingsMatch : uint8_t
    {
        /// The Scene View uses the game camera's settings.
        Same,
        /// They differ only in settings that both sides keep at their own defaults. This counts as matched.
        Defaults,
        /// They differ in a setting that one side changed from its own default.
        Differ,
    };

    /// What a measured gap depends on other than the light: the two exposure settings and the game camera.
    struct ExposureSetup
    {
        SceneViewSettings::Exposure SceneSettings;
        /// The game camera's exposure settings in the Scene View's terms.
        SceneViewSettings::Exposure GameSettings;
        ECS::EntityHandle GameCamera;

        bool operator==(const ExposureSetup&) const = default;
    };

    /// One frame's comparison of a Scene View with the game camera.
    struct Comparison
    {
        SettingsMatch Settings = SettingsMatch::Differ;
        /// The EV100 each image is exposed at, compensation included; empty without a current reading. A
        /// lower EV100 shows the same light brighter.
        std::optional<float> SceneEv;
        std::optional<float> GameEv;
        ExposureSetup Setup;
    };

    /// The message of the last frame with a current reading on both sides, and the setup it was measured in.
    struct Measurement
    {
        Message Said = Message::None;
        ExposureSetup Setup;
    };

    /// The newest metered sample of the game camera seen so far: the frame it was metered in, and when
    /// that frame replaced an earlier one. AdvancedAt is empty for the first sample seen.
    struct GameSample
    {
        uint64_t Frame = 0;
        std::optional<std::chrono::steady_clock::time_point> AdvancedAt;
    };

    explicit SceneViewExposureHintOverlay(const EditorContext& context);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

    /// What the notice says for `comparison` while it says `shown`. A current reading on both sides
    /// measures the gap and records it in `last`. Without one, `last` holds while its setup is unchanged;
    /// after a change, the settings decide.
    static Message Decide(const Comparison& comparison, Message shown, std::optional<Measurement>& last);

    /// The notice's sentence for `message`; empty for Message::None.
    static std::string Describe(Message message);

    /// Records the game camera's newest metered sample, metered in `frame`, into `sample`, and returns
    /// whether it is current at `now`: its frame replaced an earlier one within kFreshGameReadingAge.
    static bool ObserveGameSample(std::optional<GameSample>& sample, uint64_t frame,
                                  std::chrono::steady_clock::time_point now);

    /// How the Scene View's exposure settings `scene` compare with `gameCamera`'s.
    static SettingsMatch CompareSettings(const SceneViewSettings::Exposure& scene, const Components::Camera& gameCamera);

    /// Gives the Scene View `gameCamera`'s exposure settings for this session: Fixed at its EV100 for a
    /// camera that does not meter, Auto with its meter and range for one that does, and its compensation.
    static void MatchGameCamera(const Components::Camera& gameCamera);

  private:
    struct Hint
    {
        UIElement::WeakRef<EditorUI::InspectorNotice> Notice;
        ViewOverlayCamera Camera;
        Message Shown = Message::None;
        std::optional<Measurement> Last;
    };

    Message Read(Hint& hint);
    std::optional<float> CurrentGameEv(const Components::Camera& gameCamera);
    void MatchGameCameraOf(const ViewOverlayCamera& camera);
    void Dismiss();
    static void Show(Hint& hint, Message message);

    const EditorContext& m_Context;
    std::vector<Hint> m_Hints;
    std::optional<GameSample> m_GameSample;
    // Set by Dismiss, for every Scene View, until the editor restarts.
    bool m_Dismissed = false;
};

} // namespace GameEngine::Editor
