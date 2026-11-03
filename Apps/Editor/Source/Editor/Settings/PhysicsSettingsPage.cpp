#include "Editor/Settings/PhysicsSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/PhysicsProjectSettings.h"
#include "Physics/PhysicsTypes.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr float kMinGravity = -1000.0f;
constexpr float kMaxGravity = 1000.0f;
constexpr float kMinFixedTimeStep = 0.0001f;
constexpr float kMaxFixedTimeStep = 1.0f;
constexpr int kMinSteps = 1;
constexpr int kMaxSteps = 128;

using Settings = Physics::PhysicsWorldSettings;

Settings LoadSettings()
{
    return PhysicsProjectSettings::Load(EngineCore::GetInstance().GetWorkspaceRoot());
}

// Every row reads the stored block fresh and writes back only the four keys
// the page owns. The values reach the physics world the next time one is
// created: PhysicsWorldBootstrapSystem reads the settings only at
// construction, so there is no live apply and no Preview.
void SaveSettings(const Settings& settings)
{
    (void)PhysicsProjectSettings::SaveSimulationSettings(EngineCore::GetInstance().GetWorkspaceRoot(),
                                                         settings);
}

SettingsFieldDescriptor MakeGravityRow(const char* axis, float Mathematics::Vector3::*component)
{
    const Settings defaults{};

    SettingsFieldDescriptor field;
    field.Label = axis;
    field.SearchKeywords = std::string("physics gravity simulation world ") + axis;
    SettingsFieldDescriptor::NumberField number;
    number.DefaultValue = defaults.gravity.*component;
    number.MinValue = kMinGravity;
    number.MaxValue = kMaxGravity;
    number.Get = [component]() { return LoadSettings().gravity.*component; };
    number.Set = [component](float value)
    {
        Settings settings = LoadSettings();
        settings.gravity.*component = value;
        SaveSettings(settings);
    };
    field.Control = std::move(number);
    return field;
}

SettingsFieldDescriptor MakeStepCountRow(const char* label, const char* searchKeywords,
                                         int32 Settings::*member)
{
    const Settings defaults{};

    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = searchKeywords;
    SettingsFieldDescriptor::IntegerField integer;
    integer.DefaultValue = defaults.*member;
    integer.MinValue = kMinSteps;
    integer.MaxValue = kMaxSteps;
    integer.Get = [member]() { return static_cast<int>(LoadSettings().*member); };
    integer.Set = [member](int value)
    {
        Settings settings = LoadSettings();
        settings.*member = value;
        SaveSettings(settings);
    };
    field.Control = std::move(integer);
    return field;
}
} // namespace

void RegisterPhysicsSettingsCategory()
{
    SettingsCategoryDescriptor physics;
    physics.CategoryId = "physics";
    physics.Title = "Physics";
    physics.Group = SettingsCategoryGroup::ProjectSettings;
    physics.TreeRowClass = "physics-row";
    physics.SearchKeywords = "physics gravity simulation world stepping";

    SettingsFieldDescriptor gravityX = MakeGravityRow("X", &Mathematics::Vector3::x);
    gravityX.SectionHeader = "Gravity";
    physics.Fields.push_back(std::move(gravityX));
    physics.Fields.push_back(MakeGravityRow("Y", &Mathematics::Vector3::y));
    physics.Fields.push_back(MakeGravityRow("Z", &Mathematics::Vector3::z));

    {
        const Settings defaults{};
        SettingsFieldDescriptor timeStep;
        timeStep.Label = "Fixed Time Step";
        timeStep.SearchKeywords = "physics time step simulation stepping";
        timeStep.SectionHeader = "Simulation";
        SettingsFieldDescriptor::NumberField number;
        number.DefaultValue = defaults.fixedTimeStep;
        number.MinValue = kMinFixedTimeStep;
        number.MaxValue = kMaxFixedTimeStep;
        number.Get = []() { return LoadSettings().fixedTimeStep; };
        number.Set = [](float value)
        {
            Settings settings = LoadSettings();
            settings.fixedTimeStep = value;
            SaveSettings(settings);
        };
        timeStep.Control = std::move(number);
        physics.Fields.push_back(std::move(timeStep));
    }

    physics.Fields.push_back(
        MakeStepCountRow("Max Sub Steps", "physics substeps simulation", &Settings::maxSubSteps));
    physics.Fields.push_back(MakeStepCountRow("Collision Steps", "physics collision iterations",
                                              &Settings::collisionSteps));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(physics));
}

} // namespace GameEngine::Editor
