// The Animation settings page's registration shape: the rows, their reset
// defaults, and that the rows are backed by AnimationWindowSettings rather
// than a second copy of its preference keys.

#include "Editor/Settings/AnimationSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "UI/AnimationWindowSettings.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;

TEST(AnimationSettingsPageTest, RegistersRowsUnderProjectSettings)
{
    ed::RegisterAnimationSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("animation", category));

    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(category.Title, "Animation");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(category.TreeRowClass, "animation-row");

    const std::vector<std::string> expectedLabels = {
        "Keyframe Color",      "Keyframe Selected Color", "X Axis Color",       "Y Axis Color",
        "Z Axis Color",        "W Axis Color",            "Grid H-Line Color",  "Grid V-Line Color",
        "Baseline Color",      "Grid Line Thickness",     "Baseline Thickness", "Curve Line Width",
        "Default FPS",         "Show Grid",               "Show Bone Icons",    "Snap Time",
        "Snap Value",          "Properties Pane on Right", "Ruler at Top"};
    ASSERT_EQ(category.Fields.size(), expectedLabels.size());
    for (size_t i = 0; i < expectedLabels.size(); ++i)
    {
        EXPECT_EQ(category.Fields[i].Label, expectedLabels[i]);
        // AnimationWindowSettings saves every row itself.
        EXPECT_TRUE(category.Fields[i].PrefKey.empty()) << expectedLabels[i];
    }
    EXPECT_EQ(category.Fields[0].SectionHeader, "Curve Appearance");
    EXPECT_EQ(category.Fields[12].SectionHeader, "Timeline");
    EXPECT_EQ(category.Fields[13].SectionHeader, "Defaults");

    const auto& keyframe = std::get<ed::SettingsFieldDescriptor::ColorField>(category.Fields[0].Control);
    EXPECT_EQ(keyframe.DefaultArgb, 0xFF33CC80u);
    EXPECT_TRUE(keyframe.ShowHexField);
    EXPECT_TRUE(static_cast<bool>(keyframe.Preview));

    const auto& fps = std::get<ed::SettingsFieldDescriptor::SliderField>(category.Fields[12].Control);
    EXPECT_FLOAT_EQ(fps.DefaultValue, 60.0f);
    EXPECT_FLOAT_EQ(fps.MinValue, 1.0f);
    EXPECT_FLOAT_EQ(fps.MaxValue, 240.0f);
    EXPECT_TRUE(static_cast<bool>(fps.Preview));

    EXPECT_TRUE(std::get<ed::SettingsFieldDescriptor::ToggleField>(category.Fields[18].Control).DefaultValue);
}

namespace
{
namespace fs = std::filesystem;

void SetUserDataRoot(const std::string& root)
{
#ifdef _WIN32
    _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.c_str());
#else
    if (root.empty())
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
    else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.c_str(), 1);
#endif
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Points the editor preferences at an empty directory for one test and puts
// the previous root back afterwards. The settings are not copyable (they own
// their Changed event), so the fixture restores the two fields the test edits.
class AnimationSettingsPageOpenTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            m_PreviousRoot = previous;
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_animation_page_open");
        SetUserDataRoot(m_Root.string());
        const auto& settings = GameEngine::AnimationWindowSettings::Get();
        m_SavedKeyframeColor = settings.KeyframeColor;
        m_SavedShowGrid = settings.ShowGrid;
    }

    void TearDown() override
    {
        auto& settings = GameEngine::AnimationWindowSettings::Get();
        settings.KeyframeColor = m_SavedKeyframeColor;
        settings.ShowGrid = m_SavedShowGrid;
        SetUserDataRoot(m_PreviousRoot);
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    fs::path m_Root;
    std::string m_PreviousRoot;
    uint32_t m_SavedKeyframeColor = 0;
    bool m_SavedShowGrid = false;
};
} // namespace

// Opening the page runs every toggle's Set with its own Get. That must not
// save: a save writes every in-memory animation setting to the preferences
// file, so a value previewed but not committed would be persisted by merely
// opening the page.
TEST_F(AnimationSettingsPageOpenTest, OpeningThePageDoesNotSaveThePreferences)
{
    auto& settings = GameEngine::AnimationWindowSettings::Get();
    settings.Save();
    const fs::path prefsFile = m_Root / "Preferences.json";
    ASSERT_TRUE(fs::exists(prefsFile)) << "the fixture must write the preferences under the test root";
    const std::string before = ReadFile(prefsFile);

    // An uncommitted change, as a colour preview leaves it: any save from here
    // on writes it to the file.
    settings.KeyframeColor ^= 0x00FFFFFFu;

    ed::RegisterAnimationSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("animation", category));
    size_t toggles = 0;
    for (const auto& field : category.Fields)
    {
        if (const auto* toggle = std::get_if<ed::SettingsFieldDescriptor::ToggleField>(&field.Control))
        {
            toggle->Set(toggle->Get());
            ++toggles;
        }
    }
    ASSERT_GT(toggles, 0u);
    EXPECT_EQ(ReadFile(prefsFile), before) << "opening the Animation page saved the preferences";

    // The instrument: a real toggle edit does reach the file.
    const auto& showGrid = std::get<ed::SettingsFieldDescriptor::ToggleField>(category.Fields[13].Control);
    showGrid.Set(!showGrid.Get());
    EXPECT_NE(ReadFile(prefsFile), before) << "a toggle edit must save";
}

// A reset puts back the value a fresh install starts from.
TEST(AnimationSettingsPageTest, ResetDefaultsAreTheSettingsDefaults)
{
    ed::RegisterAnimationSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("animation", category));
    const GameEngine::AnimationWindowSettings& defaults = GameEngine::AnimationWindowSettings::Defaults();

    const auto color = [&](size_t i) { return std::get<ed::SettingsFieldDescriptor::ColorField>(category.Fields[i].Control).DefaultArgb; };
    const auto slider = [&](size_t i) { return std::get<ed::SettingsFieldDescriptor::SliderField>(category.Fields[i].Control).DefaultValue; };
    EXPECT_EQ(color(6), defaults.GridHLineColor);
    EXPECT_EQ(color(7), defaults.GridVLineColor);
    EXPECT_EQ(color(8), defaults.BaselineColor);
    EXPECT_FLOAT_EQ(slider(10), defaults.BaselineThickness);
}
