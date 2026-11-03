// The Physics settings page: its registration shape, and that a committed row
// writes exactly its own key into the project's physics block.

#include "Editor/Settings/PhysicsSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/PhysicsProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <variant>

namespace
{
namespace ed = GameEngine::Editor;
namespace fs = std::filesystem;

const ed::SettingsFieldDescriptor* FindField(const ed::SettingsCategoryDescriptor& category,
                                             const std::string& label)
{
    for (const auto& field : category.Fields)
    {
        if (field.Label == label)
            return &field;
    }
    return nullptr;
}

class PhysicsSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_WorkspaceRoot = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_physics_settings_page");
        fs::create_directories(m_WorkspaceRoot / ".Editor");
        GameEngine::EngineCore::GetInstance().SetWorkspaceRoot(m_WorkspaceRoot);

        ed::RegisterPhysicsSettingsCategory();
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("physics", m_Category));
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_WorkspaceRoot, ec);
    }

    fs::path m_WorkspaceRoot;
    ed::SettingsCategoryDescriptor m_Category;
};

TEST_F(PhysicsSettingsPageTest, RegistersGravityAndSimulationRowsUnderProjectSettings)
{
    EXPECT_EQ(m_Category.Title, "Physics");
    EXPECT_EQ(m_Category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(m_Category.TreeRowClass, "physics-row");
    ASSERT_EQ(m_Category.Fields.size(), 6u);
    EXPECT_EQ(m_Category.Fields[0].SectionHeader, "Gravity");
    EXPECT_EQ(m_Category.Fields[3].SectionHeader, "Simulation");

    for (const char* axis : {"X", "Y", "Z"})
    {
        const auto* field = FindField(m_Category, axis);
        ASSERT_NE(field, nullptr) << axis;
        ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::NumberField>(field->Control));
        const auto& number = std::get<ed::SettingsFieldDescriptor::NumberField>(field->Control);
        EXPECT_FLOAT_EQ(number.MinValue, -1000.0f);
        EXPECT_FLOAT_EQ(number.MaxValue, 1000.0f);
        // No live apply: a physics world reads these only when it is created.
        EXPECT_FALSE(static_cast<bool>(number.Preview));
    }
    const auto& gravityY = std::get<ed::SettingsFieldDescriptor::NumberField>(FindField(m_Category, "Y")->Control);
    EXPECT_FLOAT_EQ(gravityY.DefaultValue, -9.81f);

    for (const char* label : {"Max Sub Steps", "Collision Steps"})
    {
        const auto* field = FindField(m_Category, label);
        ASSERT_NE(field, nullptr) << label;
        ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::IntegerField>(field->Control));
        const auto& integer = std::get<ed::SettingsFieldDescriptor::IntegerField>(field->Control);
        EXPECT_EQ(integer.MinValue, 1);
        EXPECT_EQ(integer.MaxValue, 128);
    }
}

TEST_F(PhysicsSettingsPageTest, CommittingARowWritesThatValueAndKeepsTheOthers)
{
    const auto* gravityY = FindField(m_Category, "Y");
    const auto* subSteps = FindField(m_Category, "Max Sub Steps");
    ASSERT_NE(gravityY, nullptr);
    ASSERT_NE(subSteps, nullptr);

    std::get<ed::SettingsFieldDescriptor::NumberField>(gravityY->Control).Set(-3.5f);
    std::get<ed::SettingsFieldDescriptor::IntegerField>(subSteps->Control).Set(7);

    const auto stored = ed::PhysicsProjectSettings::Load(m_WorkspaceRoot);
    EXPECT_FLOAT_EQ(stored.gravity.y, -3.5f);
    EXPECT_EQ(stored.maxSubSteps, 7);
    EXPECT_FLOAT_EQ(stored.gravity.x, 0.0f);
    EXPECT_EQ(std::get<ed::SettingsFieldDescriptor::NumberField>(gravityY->Control).Get(), -3.5f);
    EXPECT_EQ(std::get<ed::SettingsFieldDescriptor::IntegerField>(subSteps->Control).Get(), 7);
}

} // namespace
