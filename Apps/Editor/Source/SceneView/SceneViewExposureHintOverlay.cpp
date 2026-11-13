#include "SceneView/SceneViewExposureHintOverlay.h"

#include "Components/Rendering/Camera.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "EditorContext.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "SceneView/SceneViewController.h"
#include "UI/Controls/InspectorNotice.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";
constexpr const char* kStyleAssetPath = "UI/controls/ViewOverlay/SceneViewExposureHint.css";
constexpr const char* kMatchLabel = "Match game camera";
constexpr const char* kMatchTooltip =
    "Use the game camera's exposure settings in this view until the editor restarts, unless you change them yourself";
constexpr const char* kDismissTooltip = "Hide this notice until the editor restarts";
// Two EV100 settings closer than this are the same setting: the exposure sliders step by 0.1, and a
// value that went through the preferences file comes back within float rounding of itself.
constexpr float kSameSettingEv = 1.0e-3f;

bool SameSetting(float a, float b)
{
    return std::abs(a - b) < kSameSettingEv;
}

bool SameExposure(const SceneViewSettings::Exposure& a, const SceneViewSettings::Exposure& b)
{
    return a.Mode == b.Mode && a.Meter == b.Meter && a.AdaptationDelay == b.AdaptationDelay &&
           SameSetting(a.FixedEv, b.FixedEv) && SameSetting(a.AutoMinEv, b.AutoMinEv) &&
           SameSetting(a.AutoMaxEv, b.AutoMaxEv) && SameSetting(a.Compensation, b.Compensation);
}

// The EV100 a camera that does not meter (Manual, Physical, Fixed) exposes at before its compensation;
// empty under Auto.
std::optional<float> UnmeteredBaseEv(const Components::Camera& camera)
{
    if (camera.ExposureControl == Components::ExposureMode::Auto)
        return std::nullopt;
    const float ev100 = Rendering::ExposureEv100ForMode(camera.ExposureControl, camera.ManualExposureEV,
                                                        camera.Aperture, camera.ShutterTime, camera.Iso);
    return Rendering::LinearExposureToEv(
        Rendering::ResolveExposureScale(camera.ExposureControl, camera.Exposure, ev100, 0.0f));
}

// `exposure` with `camera`'s exposure settings in place of its own: Fixed at the camera's EV100 for a
// camera that does not meter, Auto with its meter and range for one that does, its compensation either
// way. What the view has no field for (the camera's compensation curve, its adaptation speeds) stays.
SceneViewSettings::Exposure WithGameCameraExposure(SceneViewSettings::Exposure exposure,
                                                   const Components::Camera& camera)
{
    exposure.Compensation = camera.ExposureCompensation;
    if (const std::optional<float> base = UnmeteredBaseEv(camera))
    {
        exposure.Mode = SceneViewExposureMode::Fixed;
        exposure.FixedEv = *base;
        return exposure;
    }
    exposure.Mode = SceneViewExposureMode::Auto;
    exposure.Meter = camera.AutoExposureMeter;
    exposure.AutoMinEv = camera.AutoExposureMinEv;
    exposure.AutoMaxEv = camera.AutoExposureMaxEv;
    return exposure;
}

// `game` with the Scene View's value in each setting that both sides keep at their own defaults, so that
// only a setting someone changed can differ. A default view and a default camera differ in meter and ceiling.
SceneViewSettings::Exposure WithoutDefaultDifferences(const SceneViewSettings::Exposure& scene,
                                                      SceneViewSettings::Exposure game)
{
    const SceneViewSettings::Exposure sceneDefault;
    const SceneViewSettings::Exposure gameDefault = WithGameCameraExposure(sceneDefault, Components::Camera{});
    if (scene.Mode == sceneDefault.Mode && game.Mode == gameDefault.Mode)
        game.Mode = scene.Mode;
    if (scene.Meter == sceneDefault.Meter && game.Meter == gameDefault.Meter)
        game.Meter = scene.Meter;
    if (SameSetting(scene.FixedEv, sceneDefault.FixedEv) && SameSetting(game.FixedEv, gameDefault.FixedEv))
        game.FixedEv = scene.FixedEv;
    if (SameSetting(scene.AutoMinEv, sceneDefault.AutoMinEv) && SameSetting(game.AutoMinEv, gameDefault.AutoMinEv))
        game.AutoMinEv = scene.AutoMinEv;
    if (SameSetting(scene.AutoMaxEv, sceneDefault.AutoMaxEv) && SameSetting(game.AutoMaxEv, gameDefault.AutoMaxEv))
        game.AutoMaxEv = scene.AutoMaxEv;
    if (SameSetting(scene.Compensation, sceneDefault.Compensation) &&
        SameSetting(game.Compensation, gameDefault.Compensation))
        game.Compensation = scene.Compensation;
    return game;
}

// The EV100 the Scene View's image is exposed at: its pinned EV100 under Fixed, its metered exposure
// under Auto (empty until it meters). Compensation brightens, so it lowers the EV100; the metered value
// is the one the camera popover shows, which adds the compensation back for the switch to Fixed.
std::optional<float> SceneViewEv(const SceneViewController& controller)
{
    const SceneViewSettings& settings = SceneViewSettings::Get();
    const float compensation = settings.GetPostFxExposureCompensation();
    if (settings.GetExposureMode() == SceneViewExposureMode::Fixed)
        return settings.GetPostFxFixedExposureEv() - compensation;
    const std::optional<float> shownEv = controller.GetMeteredExposureEv();
    if (!shownEv)
        return std::nullopt;
    return *shownEv - compensation;
}

// A view's metered exposure: the frame it was metered in and the linear scale it resolved to.
struct MeteredSample
{
    uint64_t Frame = 0;
    float Scale = 0.0f;
};

// The newest metered sample among the game-purpose views (the Game View); empty while none has one.
// Each sample keeps the frame it was metered in (ExposureReadbackFeature), however long ago that was.
std::optional<MeteredSample> NewestGameSample(Engine::Renderer::RenderServices* services)
{
    auto* readback = services ? services->GetFeature<Engine::Renderer::ExposureReadbackFeature>() : nullptr;
    if (!readback)
        return std::nullopt;
    std::optional<MeteredSample> newest;
    for (const Rendering::ViewDesc& view : services->Views().GetViews())
    {
        MeteredSample sample;
        if (view.purpose != Rendering::ViewPurpose::Game ||
            !readback->TryResolveAdaptedExposure(view.id, sample.Scale, &sample.Frame))
            continue;
        if (!newest || sample.Frame > newest->Frame)
            newest = sample;
    }
    return newest;
}

// The message for a measured gap of `sceneMinusGame` EV100 while the notice says `shown`. A lower EV100
// shows the same light brighter.
SceneViewExposureHintOverlay::Message MeasuredMessage(float sceneMinusGame,
                                                      SceneViewExposureHintOverlay::Message shown)
{
    using Message = SceneViewExposureHintOverlay::Message;
    const float gap = std::abs(sceneMinusGame);
    const bool showingGap = shown == Message::Brighter || shown == Message::Darker;
    const bool shows = showingGap ? gap >= SceneViewExposureHintOverlay::kHideBelowStops
                                  : gap > SceneViewExposureHintOverlay::kShowAboveStops;
    if (!shows)
        return Message::None;
    return sceneMinusGame < 0.0f ? Message::Brighter : Message::Darker;
}

} // namespace

SceneViewExposureHintOverlay::SceneViewExposureHintOverlay(const EditorContext& context)
    : m_Context(context)
{
}

void SceneViewExposureHintOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto notice = std::make_unique<EditorUI::InspectorNotice>(std::string(), EditorUI::InspectorNotice::Kind::Information);
    notice->AddClass("inspector-notice-opaque");
    notice->AddClass("scene-view-exposure-hint");
    notice->AddClass(kHiddenClass);
    notice->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    notice->SetAction(kMatchLabel, kMatchTooltip, [this, camera]() { MatchGameCameraOf(camera); });
    notice->SetDismiss(kDismissTooltip, [this]() { Dismiss(); });
    m_Hints.push_back({UIElement::MakeWeakRef(notice.get()), camera, Message::None});
    layer.AddChild(std::move(notice));
}

void SceneViewExposureHintOverlay::Update()
{
    std::erase_if(m_Hints, [](const Hint& hint) { return hint.Notice.Get() == nullptr; });
    for (Hint& hint : m_Hints)
        Show(hint, m_Dismissed ? Message::None : Read(hint));
}

SceneViewExposureHintOverlay::Message SceneViewExposureHintOverlay::Decide(const Comparison& comparison, Message shown,
                                                                           std::optional<Measurement>& last)
{
    if (comparison.Settings == SettingsMatch::Same)
        return Message::None;
    if (comparison.SceneEv && comparison.GameEv)
    {
        const Message measured = MeasuredMessage(*comparison.SceneEv - *comparison.GameEv, shown);
        last = Measurement{measured, comparison.Setup};
        return measured;
    }
    if (last && last->Setup == comparison.Setup)
        return last->Said;
    last.reset();
    return comparison.Settings == SettingsMatch::Differ ? Message::SettingsDiffer : Message::None;
}

std::string SceneViewExposureHintOverlay::Describe(Message message)
{
    switch (message)
    {
    case Message::Brighter:
        return "This view is exposed brighter than the game camera, so the game will look darker.";
    case Message::Darker:
        return "This view is exposed darker than the game camera, so the game will look brighter.";
    case Message::SettingsDiffer:
        return "This view's exposure settings differ from the game camera's, so the game may look different.";
    case Message::None:
        break;
    }
    return {};
}

bool SceneViewExposureHintOverlay::ObserveGameSample(std::optional<GameSample>& sample, uint64_t frame,
                                                     std::chrono::steady_clock::time_point now)
{
    if (!sample)
    {
        sample = GameSample{frame, std::nullopt};
        return false;
    }
    if (sample->Frame != frame)
        *sample = GameSample{frame, now};
    return sample->AdvancedAt && now - *sample->AdvancedAt <= kFreshGameReadingAge;
}

SceneViewExposureHintOverlay::SettingsMatch SceneViewExposureHintOverlay::CompareSettings(
    const SceneViewSettings::Exposure& scene, const Components::Camera& gameCamera)
{
    const SceneViewSettings::Exposure game = WithGameCameraExposure(scene, gameCamera);
    if (SameExposure(scene, game))
        return SettingsMatch::Same;
    if (SameExposure(scene, WithoutDefaultDifferences(scene, game)))
        return SettingsMatch::Defaults;
    return SettingsMatch::Differ;
}

void SceneViewExposureHintOverlay::MatchGameCamera(const Components::Camera& gameCamera)
{
    SceneViewSettings& settings = SceneViewSettings::Get();
    settings.UseExposureForSession(WithGameCameraExposure(settings.GetExposure(), gameCamera));
}

// What the notice on `hint`'s Scene View says this frame.
SceneViewExposureHintOverlay::Message SceneViewExposureHintOverlay::Read(Hint& hint)
{
    SceneViewController* controller = hint.Camera ? hint.Camera() : nullptr;
    if (!controller)
        return Message::None;
    const std::optional<Engine::Renderer::Camera> gameCamera = Engine::Renderer::FindActiveCamera(controller->GetWorld());
    if (!gameCamera)
        return Message::None;
    const SceneViewSettings::Exposure sceneSettings = SceneViewSettings::Get().GetExposure();
    Comparison comparison;
    comparison.Settings = CompareSettings(sceneSettings, gameCamera->params);
    comparison.SceneEv = SceneViewEv(*controller);
    comparison.GameEv = CurrentGameEv(gameCamera->params);
    comparison.Setup = {sceneSettings, WithGameCameraExposure(SceneViewSettings::Exposure{}, gameCamera->params),
                        gameCamera->entity};
    return Decide(comparison, hint.Shown, hint.Last);
}

// The game camera's EV100, while current: from its settings when it does not meter, else its newest
// metered sample in the Game View while that view keeps metering. Compensation lowers the EV100; a
// metered sample includes it.
std::optional<float> SceneViewExposureHintOverlay::CurrentGameEv(const Components::Camera& gameCamera)
{
    if (const std::optional<float> base = UnmeteredBaseEv(gameCamera))
    {
        m_GameSample.reset();
        return *base - gameCamera.ExposureCompensation;
    }
    const std::optional<MeteredSample> sample = NewestGameSample(m_Context.RenderServices);
    if (!sample)
    {
        m_GameSample.reset();
        return std::nullopt;
    }
    if (!ObserveGameSample(m_GameSample, sample->Frame, std::chrono::steady_clock::now()))
        return std::nullopt;
    return Rendering::LinearExposureToEv(sample->Scale);
}

void SceneViewExposureHintOverlay::MatchGameCameraOf(const ViewOverlayCamera& camera)
{
    SceneViewController* controller = camera ? camera() : nullptr;
    if (!controller)
        return;
    const std::optional<Engine::Renderer::Camera> gameCamera = Engine::Renderer::FindActiveCamera(controller->GetWorld());
    if (gameCamera)
        MatchGameCamera(gameCamera->params);
}

void SceneViewExposureHintOverlay::Dismiss()
{
    m_Dismissed = true;
    for (Hint& hint : m_Hints)
    {
        hint.Shown = Message::None;
        if (EditorUI::InspectorNotice* notice = hint.Notice.Get())
            notice->AddClass(kHiddenClass);
    }
}

void SceneViewExposureHintOverlay::Show(Hint& hint, Message message)
{
    EditorUI::InspectorNotice* notice = hint.Notice.Get();
    if (!notice || message == hint.Shown)
        return;
    hint.Shown = message;
    notice->SetText(Describe(message));
    if (message == Message::None)
        notice->AddClass(kHiddenClass);
    else
        notice->RemoveClass(kHiddenClass);
}

} // namespace GameEngine::Editor
