// The Scene View's exposure settings: the defaults, the mode and the meter under their own preference keys,
// the handoff into Fixed, and the session-only exposure of Match game camera that no later save writes.
// The meter itself is the engine's (IncidentLightMeter, EnginePostProcessTests).

#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewExposureSettingsPage.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Panels/ExposureReadingFilter.h"
#include "SceneView/SceneViewAutoExposure.h"
#include "SceneView/SceneViewExposureHintOverlay.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <variant>

using namespace GameEngine;
using Editor::SceneViewExposureHintOverlay;
using Editor::SceneViewExposureMode;
using Editor::SceneViewSettings;

namespace
{
// Points the editor preferences at a fresh directory for one test and restores the Scene View's
// exposure state, and the move speed the session tests save through, afterward.
class ScopedScenePreferences
{
  public:
    explicit ScopedScenePreferences(const char* name)
        : m_Root(TestUtils::MakeUniqueTempDirectory(name))
    {
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", m_Root.string().c_str());
        const SceneViewSettings& settings = SceneViewSettings::Get();
        m_Mode = settings.GetExposureMode();
        m_Meter = settings.GetExposureMeter();
        m_MinEv = settings.GetPostFxAutoExposureMinEv();
        m_MaxEv = settings.GetPostFxAutoExposureMaxEv();
        m_FixedEv = settings.GetPostFxFixedExposureEv();
        m_Compensation = settings.GetPostFxExposureCompensation();
        m_MoveSpeed = settings.GetMoveSpeed();
    }
    ~ScopedScenePreferences()
    {
        SceneViewSettings& settings = SceneViewSettings::Get();
        // Reading the test's own preferences back ends a session-only exposure the test left in use.
        settings.Load();
        settings.SetExposureMode(m_Mode, /*fromUserSwitch=*/false);
        settings.SetExposureMeter(m_Meter);
        settings.SetPostFxAutoExposureMinEv(m_MinEv);
        settings.SetPostFxAutoExposureMaxEv(m_MaxEv);
        settings.SetPostFxFixedExposureEv(m_FixedEv);
        settings.SetPostFxExposureCompensation(m_Compensation);
        settings.SetMoveSpeed(m_MoveSpeed);
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

  private:
    std::filesystem::path m_Root;
    SceneViewExposureMode m_Mode{};
    Components::ExposureMeter m_Meter{};
    float m_MinEv = 0.0f;
    float m_MaxEv = 0.0f;
    float m_FixedEv = 0.0f;
    float m_Compensation = 0.0f;
    float m_MoveSpeed = 0.0f;
};
} // namespace

// The Scene View meters the lights by default, at once, with a game camera's floor and its ceiling at
// the slider's top; a game camera keeps its ceiling 18. The sliders reach below a moonless night, so a
// game camera set for one fits them.
TEST(SceneViewExposure, TheDefaultIsAutoWithTheLightsMeterAndTheCamerasFloor)
{
    EXPECT_EQ(SceneViewSettings::kDefaultExposureMode, SceneViewExposureMode::Auto);
    EXPECT_EQ(SceneViewSettings::kDefaultExposureMeter, Components::ExposureMeter::Lights);
    EXPECT_EQ(SceneViewSettings::kDefaultAutoExposureMinEv, Components::Camera{}.AutoExposureMinEv)
        << "the Scene View and a game camera share one floor";
    EXPECT_FLOAT_EQ(SceneViewSettings::kDefaultAutoExposureMinEv, -7.0f);
    EXPECT_EQ(SceneViewSettings::kDefaultAutoExposureMaxEv, SceneViewSettings::kMaxExposureEv);
    EXPECT_FLOAT_EQ(SceneViewSettings::kMinExposureEv, -14.0f)
        << "the sliders reach below a moonless night (the Lights meter reads about EV100 -12)";
    EXPECT_FLOAT_EQ(SceneViewSettings::kMaxExposureEv, 20.0f);
    EXPECT_FALSE(SceneViewSettings::kDefaultAutoExposureAdaptationDelay)
        << "the Scene View applies each reading at once unless the user turns the delay on";

    const Components::Camera camera{};
    EXPECT_FLOAT_EQ(camera.AutoExposureMaxEv, 18.0f);
}

// The mode and the meter are user preferences, saved by name and read back.
TEST(SceneViewExposure, TheModeAndTheMeterPersistByName)
{
    ScopedScenePreferences scope("SceneViewExposurePersist");
    SceneViewSettings& settings = SceneViewSettings::Get();

    settings.SetExposureMode(SceneViewExposureMode::Fixed, /*fromUserSwitch=*/false);
    settings.SetExposureMeter(Components::ExposureMeter::Picture);
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        ASSERT_TRUE(prefs.Load(&err)) << err;
        std::string stored;
        ASSERT_TRUE(prefs.TryGetString("sceneView.exposure", stored));
        EXPECT_EQ(stored, "fixed");
        ASSERT_TRUE(prefs.TryGetString("sceneView.exposureMeter", stored));
        EXPECT_EQ(stored, "picture");
        prefs.SetString("sceneView.exposure", "auto");
        prefs.SetString("sceneView.exposureMeter", "lights");
        ASSERT_TRUE(prefs.Save(&err)) << err;
    }
    settings.Load();
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Auto);
    EXPECT_EQ(settings.GetExposureMeter(), Components::ExposureMeter::Lights);
}

// The keys of the former three-mode exposure and its 4 / 18 clamps are not read: a preferences file
// that still holds them keeps the current values, not its old ones.
TEST(SceneViewExposure, TheFormerPreferenceKeysAreNotRead)
{
    ScopedScenePreferences scope("SceneViewExposureFormerKeys");
    SceneViewSettings& settings = SceneViewSettings::Get();
    settings.SetExposureMode(SceneViewExposureMode::Auto, /*fromUserSwitch=*/false);
    settings.SetPostFxAutoExposureMinEv(SceneViewSettings::kMinExposureEv);
    settings.SetPostFxAutoExposureMaxEv(SceneViewSettings::kMaxExposureEv);
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        ASSERT_TRUE(prefs.Load(&err)) << err;
        prefs.SetDouble("sceneView.exposureMode", 2.0); // the former Fixed
        prefs.SetDouble("sceneView.postFxAutoExposureMinEv", 4.0);
        prefs.SetDouble("sceneView.postFxAutoExposureMaxEv", 18.0);
        ASSERT_TRUE(prefs.Save(&err)) << err;
    }
    settings.Load();
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Auto);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMinEv(), SceneViewSettings::kMinExposureEv);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMaxEv(), SceneViewSettings::kMaxExposureEv);
}

// Switching from Auto to Fixed starts Fixed at the EV the view was showing, on a slider detent, so
// the image does not jump at the switch.
TEST(SceneViewExposure, SwitchingToFixedStartsAtTheShownEv)
{
    ScopedScenePreferences scope("SceneViewExposureHandoff");
    SceneViewSettings& settings = SceneViewSettings::Get();

    settings.SetExposureMode(SceneViewExposureMode::Auto, /*fromUserSwitch=*/false);
    settings.SetPostFxFixedExposureEv(9.0f);
    settings.SetLastShownExposureEv(13.37f);
    settings.SetExposureMode(SceneViewExposureMode::Fixed, /*fromUserSwitch=*/true);
    EXPECT_FLOAT_EQ(settings.GetPostFxFixedExposureEv(), 13.4f);
}

// What the Scene View publishes with its shipped settings: Auto on the Lights meter, the open clamps
// (with no volume clamp, its own floor reaches a moonless night), and both speeds at the instant
// constant, so a new reading lands on its first frame.
TEST(SceneViewExposure, TheDefaultAutoPublishesTheLightsMeterTheFloorAndInstantSpeeds)
{
    Engine::Renderer::PostProcessSettings settings{};
    Editor::ApplySceneViewAutoExposure(Editor::SceneViewAutoExposure{}, settings);
    EXPECT_TRUE(settings.AutoExposureActive);
    EXPECT_EQ(settings.AutoExposureMeter, static_cast<int32>(Components::ExposureMeter::Lights));
    EXPECT_FLOAT_EQ(settings.AutoExposureMinEv, SceneViewSettings::kDefaultAutoExposureMinEv)
        << "a view with no volume clamp keeps its own floor";
    EXPECT_FLOAT_EQ(settings.AutoExposureMaxEv, 20.0f);
    EXPECT_EQ(settings.AutoExposureSpeedUp, Editor::kInstantAdaptationSpeed);
    EXPECT_EQ(settings.AutoExposureSpeedDown, Editor::kInstantAdaptationSpeed);
    // The night follows a game camera's default compensation curve.
    const auto curve = Rendering::SampleExposureCompensation(Components::DefaultExposureCompensationCurve());
    EXPECT_EQ(std::memcmp(&settings.AutoExposureCompensation, &curve, sizeof(curve)), 0);

    // The instant speed lands the target on the first frame at a 240 Hz frame time: the resolve's
    // smoothing factor is exactly 1 in float32.
    const float dt = 1.0f / 240.0f;
    EXPECT_EQ(1.0f - std::exp2(-dt * Editor::kInstantAdaptationSpeed), 1.0f);
}

// With the delay on the world sensor's speeds stay, and a volume's Exposure Adjustment clamps narrow
// the Scene View's open envelope; the compensation adds to the bias the volume already set.
TEST(SceneViewExposure, TheDelayKeepsTheSensorSpeedsAndVolumeClampsNarrowTheEnvelope)
{
    Engine::Renderer::PostProcessSettings settings{};
    settings.ExposureClampMinEv = 2.0f;
    settings.ExposureClampMinWeight = 1.0f;
    settings.ExposureClampMaxEv = 16.0f;
    settings.ExposureClampMaxWeight = 1.0f;
    settings.AutoExposureBiasEv = 0.5f;
    const float sensorSpeedUp = settings.AutoExposureSpeedUp;
    Editor::ApplySceneViewAutoExposure(
        {.Meter = Components::ExposureMeter::Picture, .CompensationEv = 1.0f, .AdaptationDelay = true}, settings);
    EXPECT_EQ(settings.AutoExposureMeter, static_cast<int32>(Components::ExposureMeter::Picture));
    EXPECT_FLOAT_EQ(settings.AutoExposureMinEv, 2.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureMaxEv, 16.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureBiasEv, 1.5f);
    EXPECT_EQ(settings.AutoExposureSpeedUp, sensorSpeedUp);
}

// A volume's clamp fades in with its blend weight: one percent into its blend region, a volume that
// clamps the floor at EV100 2 moves the Scene View's -7 floor one percent of the way, not to the
// no-op endpoint at -5; fully inside, the floor is the volume's.
// The Scene View's published floor under one volume that clamps the floor at `authoredMinEv`, blended
// in at `weight` (the clamp and its weight blended from the no-op endpoint and 0, as volumes blend).
static float SceneViewFloorUnderAVolumeClamp(float authoredMinEv, float weight)
{
    Engine::Renderer::PostProcessSettings settings{};
    settings.ExposureClampMinEv = Engine::Renderer::kExposureClampNoOpMinEv +
                                  weight * (authoredMinEv - Engine::Renderer::kExposureClampNoOpMinEv);
    settings.ExposureClampMinWeight = weight;
    Editor::ApplySceneViewAutoExposure(Editor::SceneViewAutoExposure{}, settings);
    return settings.AutoExposureMinEv;
}

TEST(SceneViewExposure, AVolumeFadingInMovesTheFloorSmoothly)
{
    constexpr float kAuthoredMinEv = 2.0f;
    EXPECT_NEAR(SceneViewFloorUnderAVolumeClamp(kAuthoredMinEv, 0.01f), SceneViewSettings::kDefaultAutoExposureMinEv + 0.01f * (kAuthoredMinEv - SceneViewSettings::kDefaultAutoExposureMinEv),
                1e-4f)
        << "one percent into the blend region the floor moves one percent of the way";
    EXPECT_NEAR(SceneViewFloorUnderAVolumeClamp(kAuthoredMinEv, 1.0f), kAuthoredMinEv, 1e-4f) << "inside the volume the floor is the volume's";
}

// The popover's Reading line follows a live EV100 without flipping on a rounding boundary: a Picture
// reading that hovers across 14.75 refreshes the line once, and a move of a whole display step
// refreshes it again.
TEST(SceneViewExposure, AReadingHoveringAcrossARoundingBoundaryRefreshesTheLineOnce)
{
    Editor::ExposureReadingFilter filter;
    int refreshes = 0;
    for (int frame = 0; frame < 20; ++frame)
        refreshes += filter.Accept(frame % 2 == 0 ? 14.74f : 14.76f) ? 1 : 0;
    EXPECT_EQ(refreshes, 1);
    ASSERT_TRUE(filter.Shown().has_value());
    EXPECT_FLOAT_EQ(*filter.Shown(), 14.74f);

    EXPECT_TRUE(filter.Accept(14.9f)) << "a whole display step away shows the new value";
    EXPECT_FALSE(filter.Accept(std::optional<float>(14.85f)));
    EXPECT_TRUE(filter.Accept(std::nullopt)) << "losing the reading clears the line";
}

// The Scene View takes the curve it is given, the scene's main camera's: a camera with no
// compensation gives a Scene View with none.
TEST(SceneViewExposure, TheSceneViewTakesTheCamerasCurve)
{
    Editor::SceneViewAutoExposure autoExposure{};
    autoExposure.CompensationCurve = Math::Curve{};
    autoExposure.CompensationCurve.TryInsert(Math::CurveKey{-4.0f, -1.0f});
    Engine::Renderer::PostProcessSettings settings{};
    Editor::ApplySceneViewAutoExposure(autoExposure, settings);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.StartEv, -4.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.Stops[0], -1.0f);
}

namespace
{
// The exposure fields as the preferences file holds them.
struct SavedExposure
{
    std::string Meter;
    double AutoMaxEv = 0.0;
    double Compensation = 0.0;
    double MoveSpeed = 0.0;
};

SavedExposure ReadSavedExposure()
{
    SavedExposure saved;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    EXPECT_TRUE(prefs.Load(&err)) << err;
    EXPECT_TRUE(prefs.TryGetString("sceneView.exposureMeter", saved.Meter));
    EXPECT_TRUE(prefs.TryGetDouble("sceneView.autoExposureMaxEv", saved.AutoMaxEv));
    EXPECT_TRUE(prefs.TryGetDouble("sceneView.postFxExposureCompensation", saved.Compensation));
    EXPECT_TRUE(prefs.TryGetDouble("sceneView.moveSpeed", saved.MoveSpeed));
    return saved;
}

// The user's own Scene View exposure, saved: Auto on the lights meter, its ceiling at 20, no compensation.
void SaveTheUsersOwnExposure(SceneViewSettings& settings)
{
    settings.SetExposureMode(SceneViewExposureMode::Auto, /*fromUserSwitch=*/false);
    settings.SetExposureMeter(Components::ExposureMeter::Lights);
    settings.SetPostFxAutoExposureMinEv(SceneViewSettings::kMinExposureEv);
    settings.SetPostFxAutoExposureMaxEv(20.0f);
    settings.SetPostFxExposureCompensation(0.0f);
}

// A game camera that meters the picture, with its own range and compensation.
Components::Camera AnAutoGameCamera()
{
    Components::Camera camera{};
    camera.ExposureControl = Components::ExposureMode::Auto;
    camera.AutoExposureMeter = Components::ExposureMeter::Picture;
    camera.AutoExposureMinEv = -2.0f;
    camera.AutoExposureMaxEv = 16.0f;
    camera.ExposureCompensation = 0.5f;
    return camera;
}

bool UsesGameCameraExposure(const Components::Camera& camera)
{
    return SceneViewExposureHintOverlay::CompareSettings(SceneViewSettings::Get().GetExposure(), camera) ==
           SceneViewExposureHintOverlay::SettingsMatch::Same;
}

std::string ReadPreferencesFile()
{
    std::ifstream file(Editor::OpenEditorPreferences().GetFilePath(), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Builds the Exposure settings page as the Settings panel does: each slider row seeds itself with its
// getter's value clamped to the row's range (EditorSettingsRegistry.h, SliderField).
void OpenTheExposureSettingsPage()
{
    Editor::RegisterSceneViewExposureSettingsCategory();
    Editor::SettingsCategoryDescriptor page;
    ASSERT_TRUE(Editor::EditorSettingsRegistry::Get().TryGet("sceneViewExposure", page));
    for (const Editor::SettingsFieldDescriptor& field : page.Fields)
        if (const auto* slider = std::get_if<Editor::SettingsFieldDescriptor::SliderField>(&field.Control))
            slider->Set(std::clamp(slider->Get(), slider->MinValue, slider->MaxValue));
}
} // namespace

// An exposure used for this session changes what the view uses, but a later save of an unrelated setting
// (the move speed) writes the user's own exposure, and the next start reads it back.
TEST(SceneViewExposure, ASessionExposureIsNotSavedByALaterUnrelatedSave)
{
    ScopedScenePreferences scope("SceneViewExposureSession");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);

    SceneViewSettings::Exposure session = settings.GetExposure();
    session.Meter = Components::ExposureMeter::Picture;
    session.AutoMaxEv = 18.0f;
    session.Compensation = 0.5f;
    settings.UseExposureForSession(session);
    EXPECT_TRUE(settings.GetExposure() == session) << "the view does not use the session's exposure";

    settings.SetMoveSpeed(7.0f);
    const SavedExposure saved = ReadSavedExposure();
    EXPECT_DOUBLE_EQ(saved.MoveSpeed, 7.0) << "the unrelated setting was not saved, so this test proves nothing";
    EXPECT_EQ(saved.Meter, "lights") << "a later save wrote the session's meter";
    EXPECT_DOUBLE_EQ(saved.AutoMaxEv, 20.0) << "a later save wrote the session's ceiling";
    EXPECT_DOUBLE_EQ(saved.Compensation, 0.0) << "a later save wrote the session's compensation";
    EXPECT_EQ(settings.GetExposureMeter(), Components::ExposureMeter::Picture) << "the save ended the session";

    settings.Load();
    EXPECT_EQ(settings.GetExposureMeter(), Components::ExposureMeter::Lights) << "the next start kept the session's";
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMaxEv(), 20.0f);
    EXPECT_FLOAT_EQ(settings.GetPostFxExposureCompensation(), 0.0f);
}

// An exposure edit made while a session exposure is in use is the user's: the values on screen, the
// session's with the edit, are what is saved.
TEST(SceneViewExposure, AnExposureEditDuringASessionSavesWhatTheViewShows)
{
    ScopedScenePreferences scope("SceneViewExposureSessionEdit");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);

    SceneViewSettings::Exposure session = settings.GetExposure();
    session.Meter = Components::ExposureMeter::Picture;
    session.AutoMaxEv = 18.0f;
    settings.UseExposureForSession(session);
    settings.SetPostFxExposureCompensation(1.0f);

    const SavedExposure saved = ReadSavedExposure();
    EXPECT_EQ(saved.Meter, "picture");
    EXPECT_DOUBLE_EQ(saved.AutoMaxEv, 18.0);
    EXPECT_DOUBLE_EQ(saved.Compensation, 1.0);
}

// Match game camera gives the Scene View an Auto game camera's meter, range and compensation, after which
// the view uses the camera's settings; nothing is saved.
TEST(SceneViewExposure, MatchingAnAutoGameCameraTakesItsMeterRangeAndCompensation)
{
    ScopedScenePreferences scope("SceneViewExposureMatchAuto");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    settings.SetExposureMode(SceneViewExposureMode::Fixed, /*fromUserSwitch=*/false);
    const Components::Camera camera = AnAutoGameCamera();
    EXPECT_FALSE(UsesGameCameraExposure(camera));

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Auto);
    EXPECT_EQ(settings.GetExposureMeter(), Components::ExposureMeter::Picture);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMinEv(), -2.0f);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMaxEv(), 16.0f);
    EXPECT_FLOAT_EQ(settings.GetPostFxExposureCompensation(), 0.5f);
    EXPECT_TRUE(UsesGameCameraExposure(camera));

    settings.SetMoveSpeed(7.0f);
    const SavedExposure saved = ReadSavedExposure();
    EXPECT_EQ(saved.Meter, "lights") << "the match was saved";
    EXPECT_DOUBLE_EQ(saved.Compensation, 0.0) << "the match was saved";
}

// A game camera that does not meter gives the Scene View Fixed at the camera's EV100, with its
// compensation.
TEST(SceneViewExposure, MatchingAManualGameCameraPinsItsEv)
{
    ScopedScenePreferences scope("SceneViewExposureMatchManual");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    Components::Camera camera{};
    camera.ExposureControl = Components::ExposureMode::Manual;
    camera.ManualExposureEV = 12.0f;
    camera.ExposureCompensation = -1.0f;

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Fixed);
    EXPECT_NEAR(settings.GetPostFxFixedExposureEv(), 12.0f, 1e-4f);
    EXPECT_FLOAT_EQ(settings.GetPostFxExposureCompensation(), -1.0f);
    EXPECT_TRUE(UsesGameCameraExposure(camera));
}

// A second session exposure (Match game camera again, after the camera changed) keeps the user's own exposure
// as what is saved: the first session's values never become the user's.
TEST(SceneViewExposure, ASecondSessionExposureStillSavesTheUsersOwn)
{
    ScopedScenePreferences scope("SceneViewExposureSessionTwice");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);

    SceneViewSettings::Exposure first = settings.GetExposure();
    first.Meter = Components::ExposureMeter::Picture;
    first.AutoMaxEv = 18.0f;
    settings.UseExposureForSession(first);
    SceneViewSettings::Exposure second = first;
    second.Compensation = 0.5f;
    settings.UseExposureForSession(second);

    settings.SetMoveSpeed(7.0f);
    const SavedExposure saved = ReadSavedExposure();
    EXPECT_EQ(saved.Meter, "lights") << "the second session exposure made the first one the user's own";
    EXPECT_DOUBLE_EQ(saved.AutoMaxEv, 20.0) << "the second session exposure made the first one the user's own";
    EXPECT_DOUBLE_EQ(saved.Compensation, 0.0);
}

// A game camera can fully expose a darker night than the Scene View's default floor: Match takes the
// camera's floor, and the view then uses the camera's settings.
TEST(SceneViewExposure, MatchingACameraWithANightFloorTakesItsFloor)
{
    ScopedScenePreferences scope("SceneViewExposureMatchNightFloor");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    Components::Camera camera = AnAutoGameCamera();
    camera.AutoExposureMinEv = -10.0f;

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMinEv(), -10.0f) << "Match did not take the camera's floor";
    EXPECT_TRUE(UsesGameCameraExposure(camera)) << "after Match the settings still differ";
}

// A Manual camera set for a night pins the Scene View at the camera's EV100.
TEST(SceneViewExposure, MatchingAManualNightCameraPinsItsEv)
{
    ScopedScenePreferences scope("SceneViewExposureMatchManualNight");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    Components::Camera camera{};
    camera.ExposureControl = Components::ExposureMode::Manual;
    camera.ManualExposureEV = -10.0f;

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Fixed);
    EXPECT_NEAR(settings.GetPostFxFixedExposureEv(), -10.0f, 1e-3f) << "Match did not take the camera's EV100";
    EXPECT_TRUE(UsesGameCameraExposure(camera)) << "after Match the settings still differ";
}

// A camera's compensation of six stops comes over whole.
TEST(SceneViewExposure, MatchingACameraWithALargeCompensationTakesIt)
{
    ScopedScenePreferences scope("SceneViewExposureMatchLargeCompensation");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    Components::Camera camera = AnAutoGameCamera();
    camera.ExposureCompensation = 6.0f;

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_FLOAT_EQ(settings.GetPostFxExposureCompensation(), 6.0f) << "Match did not take the camera's compensation";
    EXPECT_TRUE(UsesGameCameraExposure(camera)) << "after Match the settings still differ";
}

// A camera's EV100 below the Scene View's range lands on the range's floor, and opening the Exposure
// settings page then writes nothing: every slider seeds with a value it can hold.
TEST(SceneViewExposure, MatchingACameraBelowTheRangeLandsOnTheFloorAndOpeningTheSettingsWritesNothing)
{
    ScopedScenePreferences scope("SceneViewExposureMatchBelowRange");
    SceneViewSettings& settings = SceneViewSettings::Get();
    SaveTheUsersOwnExposure(settings);
    Components::Camera camera{};
    camera.ExposureControl = Components::ExposureMode::Manual;
    camera.ManualExposureEV = -20.0f;

    SceneViewExposureHintOverlay::MatchGameCamera(camera);
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Fixed);
    EXPECT_FLOAT_EQ(settings.GetPostFxFixedExposureEv(), SceneViewSettings::kMinExposureEv)
        << "Match did not clamp the camera's EV100 to the Scene View's range";

    const std::string saved = ReadPreferencesFile();
    OpenTheExposureSettingsPage();
    EXPECT_EQ(ReadPreferencesFile(), saved) << "opening the settings page wrote the preferences";
    EXPECT_FLOAT_EQ(settings.GetPostFxFixedExposureEv(), SceneViewSettings::kMinExposureEv)
        << "opening the settings page changed the view's exposure";
}
