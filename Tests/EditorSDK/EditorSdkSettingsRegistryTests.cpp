// EditorSettingsRegistry semantics — the contracts settings-owning systems and
// package modules rely on:
//   * registration validation is loud and rejecting,
//   * same-CategoryId registration replaces forward IN PLACE so snapshot
//     indices (and therefore settings-tree ids) stay stable for the session,
//   * observers replay registrations that happened before attach.
//
// Links EditorSDK.dll — the same registry Editor.exe and module DLLs share.
// The registry is a process singleton, so every test uses unique CategoryIds.

#include "Editor/Settings/EditorSettingsRegistry.h"

// A custom row hands back a UIElement, and building one here needs the
// complete type the registry header only forward-declares.
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;

namespace
{

ed::SettingsCategoryDescriptor MakeCategory(std::string id, std::string title)
{
    ed::SettingsCategoryDescriptor descriptor;
    descriptor.CategoryId = std::move(id);
    descriptor.Title = std::move(title);

    ed::SettingsFieldDescriptor field;
    field.Label = "Enabled";
    ed::SettingsFieldDescriptor::ToggleField toggle;
    toggle.Get = []() { return true; };
    field.Control = toggle;
    descriptor.Fields.push_back(std::move(field));
    return descriptor;
}

int IndexOf(const std::vector<ed::SettingsCategoryDescriptor>& snapshot, const std::string& id)
{
    for (size_t i = 0; i < snapshot.size(); ++i)
        if (snapshot[i].CategoryId == id)
            return static_cast<int>(i);
    return -1;
}

} // namespace

TEST(EditorSdkSettingsRegistryTests, RegistrationValidationRejects)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    const size_t before = registry.Snapshot().size();

    // Missing CategoryId.
    registry.RegisterCategory(MakeCategory("", "No Id"));
    // Missing Title.
    registry.RegisterCategory(MakeCategory("settingsTestNoTitle", ""));
    // Field with neither PrefKey nor Get.
    ed::SettingsCategoryDescriptor bad = MakeCategory("settingsTestBadField", "Bad Field");
    bad.Fields[0].Control = ed::SettingsFieldDescriptor::ToggleField{};
    registry.RegisterCategory(bad);

    EXPECT_EQ(registry.Snapshot().size(), before);
}

// A value row is rejected without somewhere to read its value from, whichever
// control it uses — the text and path rows answer to the same rule as the
// toggle above.
TEST(EditorSdkSettingsRegistryTests, ValueRowsNeedAValueSource)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    const size_t before = registry.Snapshot().size();

    ed::SettingsCategoryDescriptor unwiredString = MakeCategory("settingsTestBadString", "Bad String");
    unwiredString.Fields[0].Control = ed::SettingsFieldDescriptor::StringField{};
    registry.RegisterCategory(unwiredString);

    ed::SettingsCategoryDescriptor unwiredPath = MakeCategory("settingsTestBadPath", "Bad Path");
    unwiredPath.Fields[0].Control = ed::SettingsFieldDescriptor::PathField{};
    registry.RegisterCategory(unwiredPath);

    EXPECT_EQ(registry.Snapshot().size(), before);

    // A PrefKey is a value source even with no accessor, so this one is kept.
    ed::SettingsCategoryDescriptor keyed = MakeCategory("settingsTestKeyedString", "Keyed String");
    keyed.Fields[0].PrefKey = "settingsTest.keyedString";
    keyed.Fields[0].Control = ed::SettingsFieldDescriptor::StringField{};
    registry.RegisterCategory(keyed);

    EXPECT_EQ(registry.Snapshot().size(), before + 1);
}

// Number rows answer to the same value-source rule, and keep the range, the
// default and the Preview/Set split they were registered with.
TEST(EditorSdkSettingsRegistryTests, NumberRowsNeedAValueSourceAndKeepTheirRange)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    const size_t before = registry.Snapshot().size();

    ed::SettingsCategoryDescriptor unwiredNumber = MakeCategory("settingsTestBadNumber", "Bad Number");
    unwiredNumber.Fields[0].Control = ed::SettingsFieldDescriptor::NumberField{};
    registry.RegisterCategory(unwiredNumber);

    ed::SettingsCategoryDescriptor unwiredInteger = MakeCategory("settingsTestBadInteger", "Bad Integer");
    unwiredInteger.Fields[0].Control = ed::SettingsFieldDescriptor::IntegerField{};
    registry.RegisterCategory(unwiredInteger);

    EXPECT_EQ(registry.Snapshot().size(), before);

    ed::SettingsCategoryDescriptor numbers = MakeCategory("settingsTestNumbers", "Numbers");
    ed::SettingsFieldDescriptor::NumberField number;
    number.DefaultValue = 0.5f;
    number.MinValue = 0.25f;
    number.MaxValue = 4.0f;
    number.Get = []() { return 1.0f; };
    number.Preview = [](float) {};
    number.Set = [](float) {};
    numbers.Fields[0].Control = number;

    ed::SettingsFieldDescriptor keyedInteger;
    keyedInteger.Label = "Steps";
    keyedInteger.PrefKey = "settingsTest.steps";
    ed::SettingsFieldDescriptor::IntegerField integer;
    integer.DefaultValue = 2;
    integer.MinValue = 1;
    integer.MaxValue = 128;
    keyedInteger.Control = integer;
    numbers.Fields.push_back(std::move(keyedInteger));
    registry.RegisterCategory(numbers);

    ed::SettingsCategoryDescriptor stored;
    ASSERT_TRUE(registry.TryGet("settingsTestNumbers", stored));
    ASSERT_EQ(stored.Fields.size(), 2u);
    const auto* storedNumber = std::get_if<ed::SettingsFieldDescriptor::NumberField>(&stored.Fields[0].Control);
    ASSERT_NE(storedNumber, nullptr);
    EXPECT_FLOAT_EQ(storedNumber->DefaultValue, 0.5f);
    EXPECT_FLOAT_EQ(storedNumber->MinValue, 0.25f);
    EXPECT_FLOAT_EQ(storedNumber->MaxValue, 4.0f);
    EXPECT_TRUE(static_cast<bool>(storedNumber->Preview));
    EXPECT_TRUE(static_cast<bool>(storedNumber->Set));
    const auto* storedInteger = std::get_if<ed::SettingsFieldDescriptor::IntegerField>(&stored.Fields[1].Control);
    ASSERT_NE(storedInteger, nullptr);
    EXPECT_EQ(storedInteger->DefaultValue, 2);
    EXPECT_EQ(storedInteger->MinValue, 1);
    EXPECT_EQ(storedInteger->MaxValue, 128);
}

// The info card and the visibility condition belong to the field, whatever its
// control, and reach the panel through the snapshot unchanged.
TEST(EditorSdkSettingsRegistryTests, InfoCardAndVisibilityConditionSurviveTheSnapshot)
{
    auto& registry = ed::EditorSettingsRegistry::Get();

    ed::SettingsCategoryDescriptor category = MakeCategory("settingsTestConditional", "Conditional");
    auto shown = std::make_shared<bool>(false);
    category.Fields[0].InfoCard = "Explains the row.";
    category.Fields[0].IsVisible = [shown]() { return *shown; };
    registry.RegisterCategory(category);

    ed::SettingsCategoryDescriptor stored;
    ASSERT_TRUE(registry.TryGet("settingsTestConditional", stored));
    ASSERT_EQ(stored.Fields.size(), 1u);
    EXPECT_EQ(stored.Fields[0].InfoCard, "Explains the row.");
    ASSERT_TRUE(static_cast<bool>(stored.Fields[0].IsVisible));
    EXPECT_FALSE(stored.Fields[0].IsVisible());
    *shown = true;
    EXPECT_TRUE(stored.Fields[0].IsVisible()) << "the condition is re-read, not captured at registration";
}

// Button and custom rows hold no value: they answer for their own wiring, and
// a label is optional because they carry their own.
TEST(EditorSdkSettingsRegistryTests, ActionRowsNeedWiringRatherThanALabel)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    const size_t before = registry.Snapshot().size();

    ed::SettingsCategoryDescriptor deadButton = MakeCategory("settingsTestDeadButton", "Dead Button");
    deadButton.Fields[0].Control = ed::SettingsFieldDescriptor::ButtonField{};
    registry.RegisterCategory(deadButton);

    ed::SettingsCategoryDescriptor deadCustom = MakeCategory("settingsTestDeadCustom", "Dead Custom");
    deadCustom.Fields[0].Control = ed::SettingsFieldDescriptor::CustomField{};
    registry.RegisterCategory(deadCustom);

    EXPECT_EQ(registry.Snapshot().size(), before);

    ed::SettingsCategoryDescriptor button = MakeCategory("settingsTestButton", "Button");
    button.Fields[0].Label.clear();
    ed::SettingsFieldDescriptor::ButtonField click;
    click.ButtonText = "Do The Thing";
    click.OnClick = []() {};
    button.Fields[0].Control = std::move(click);
    registry.RegisterCategory(button);

    ed::SettingsCategoryDescriptor custom = MakeCategory("settingsTestCustom", "Custom");
    custom.Fields[0].Label.clear();
    ed::SettingsFieldDescriptor::CustomField row;
    row.CreateRow = []() { return std::unique_ptr<GameEngine::UIElement>{}; };
    custom.Fields[0].Control = std::move(row);
    registry.RegisterCategory(custom);

    EXPECT_EQ(registry.Snapshot().size(), before + 2);
}

// A sub-page carries the id of the page it nests under, so the panel can
// compose the tree from the snapshot alone.
TEST(EditorSdkSettingsRegistryTests, ParentCategoryIdSurvivesTheSnapshot)
{
    auto& registry = ed::EditorSettingsRegistry::Get();

    registry.RegisterCategory(MakeCategory("settingsTestParent", "Parent"));
    ed::SettingsCategoryDescriptor child = MakeCategory("settingsTestParent.child", "Child");
    child.ParentCategoryId = "settingsTestParent";
    registry.RegisterCategory(std::move(child));

    ed::SettingsCategoryDescriptor found;
    ASSERT_TRUE(registry.TryGet("settingsTestParent.child", found));
    EXPECT_EQ(found.ParentCategoryId, "settingsTestParent");

    // A page that names no parent stays a root of its group.
    ASSERT_TRUE(registry.TryGet("settingsTestParent", found));
    EXPECT_TRUE(found.ParentCategoryId.empty());
}

TEST(EditorSdkSettingsRegistryTests, ReplaceForwardKeepsSnapshotIndex)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    registry.RegisterCategory(MakeCategory("settingsTestReplaceA", "Replace A"));
    registry.RegisterCategory(MakeCategory("settingsTestReplaceB", "Replace B"));

    auto snapshot = registry.Snapshot();
    const int indexA = IndexOf(snapshot, "settingsTestReplaceA");
    const int indexB = IndexOf(snapshot, "settingsTestReplaceB");
    ASSERT_GE(indexA, 0);
    ASSERT_GE(indexB, 0);

    // Re-register A (module reload): the NEW descriptor wins in place.
    registry.RegisterCategory(MakeCategory("settingsTestReplaceA", "Replace A v2"));
    snapshot = registry.Snapshot();
    EXPECT_EQ(IndexOf(snapshot, "settingsTestReplaceA"), indexA);
    EXPECT_EQ(IndexOf(snapshot, "settingsTestReplaceB"), indexB);
    EXPECT_EQ(snapshot[static_cast<size_t>(indexA)].Title, "Replace A v2");

    ed::SettingsCategoryDescriptor fetched;
    ASSERT_TRUE(registry.TryGet("settingsTestReplaceA", fetched));
    EXPECT_EQ(fetched.Title, "Replace A v2");
}

TEST(EditorSdkSettingsRegistryTests, ObserverReplaysExistingRegistrations)
{
    auto& registry = ed::EditorSettingsRegistry::Get();
    registry.RegisterCategory(MakeCategory("settingsTestReplay", "Replay"));

    std::vector<std::string> seen;
    registry.SetRegistrationObserver([&seen](const ed::SettingsCategoryDescriptor& descriptor)
    {
        seen.push_back(descriptor.CategoryId);
    });

    // Replay covered the pre-attach registration.
    EXPECT_NE(IndexOf([&] {
                  std::vector<ed::SettingsCategoryDescriptor> snapshot;
                  for (const auto& id : seen)
                  {
                      ed::SettingsCategoryDescriptor d;
                      d.CategoryId = id;
                      snapshot.push_back(d);
                  }
                  return snapshot;
              }(),
              "settingsTestReplay"),
              -1);

    // New registrations notify as they land.
    seen.clear();
    registry.RegisterCategory(MakeCategory("settingsTestLive", "Live"));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front(), "settingsTestLive");

    registry.SetRegistrationObserver({});
}
