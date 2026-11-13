// The inspector's reference fields show the referenced object, never its GUID: the asset's
// name and glyph, the GUID only in the name's tooltip, a missing state for a GUID that names
// no asset, a picker button beside the clear button, and a click that reveals the asset.

#include <gtest/gtest.h>

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Types/StringUtils.h"
#include "UI/AssetField.h"
#include "UI/Controls/Label.h"
#include "UI/EntityField.h"
#include "UI/UIEvents.h"

#include <filesystem>
#include <string>

using namespace GameEngine;

namespace
{

EngineCore& StartedEngine()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }
    return engine;
}

// A prefab in the registry, with no file behind it: the field reads metadata only.
GUID RegisterPrefab(AssetRegistry& registry, const std::string& name)
{
    AssetMetadata meta{};
    meta.Guid = GUID::Generate();
    meta.Type = AssetType::Prefab;
    meta.Name = name;
    meta.Extension = ".prefab";
    meta.Path = registry.GetAssetRoot() / "Prefabs" / (name + ".prefab");
    EXPECT_TRUE(registry.RegisterAssetMetadata(meta));
    return meta.Guid;
}

UIElement* ChildWithClass(UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child && child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

Label& NameLabel(UIElement& field)
{
    auto* label = dynamic_cast<Label*>(ChildWithClass(field, "asset-field-name"));
    EXPECT_NE(label, nullptr);
    return *label;
}

} // namespace

TEST(ReferenceFieldTests, APrefabShowsItsNameAndThePrefabGlyph)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID crate = RegisterPrefab(registry, "Crate");

    AssetField field;
    field.SetAcceptedTypes({AssetType::Prefab});
    field.SetAssetRegistry(&registry);
    field.SetValue(crate);

    EXPECT_EQ(NameLabel(field).GetText(), "Crate");
    UIElement* icon = ChildWithClass(field, "asset-field-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("icon-prefab"));
    EXPECT_FALSE(icon->HasClass("hidden"));
}

TEST(ReferenceFieldTests, AnEmptyFieldShowsNoIcon)
{
    AssetField field;
    field.SetValue(GUID::Null());

    EXPECT_EQ(NameLabel(field).GetText(), "None");
    UIElement* icon = ChildWithClass(field, "asset-field-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("hidden")) << "an empty field leaves no gap before None";
}

TEST(ReferenceFieldTests, TheGuidIsInTheNameTooltipNeverInTheText)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID crate = RegisterPrefab(registry, "Barrel");

    AssetField field;
    field.SetAssetRegistry(&registry);
    field.SetValue(crate);

    const Label& name = NameLabel(field);
    EXPECT_EQ(name.GetText().find(crate.ToString()), std::string::npos);
    EXPECT_EQ(name.GetTooltip(), "Barrel.prefab\nGUID " + crate.ToString());
}

TEST(ReferenceFieldTests, AGuidThatNamesNoAssetShowsMissingNotTheGuid)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID unknown = GUID::Generate();

    AssetField field;
    field.SetAssetRegistry(&registry);
    field.SetValue(unknown);

    const Label& name = NameLabel(field);
    EXPECT_EQ(name.GetText(), "Missing asset");
    EXPECT_TRUE(name.HasClass("missing"));
    EXPECT_EQ(name.GetText().find(unknown.ToString().substr(0, 8)), std::string::npos);
    EXPECT_NE(name.GetTooltip().find(unknown.ToString()), std::string::npos);
    UIElement* clear = ChildWithClass(field, "asset-field-clear");
    ASSERT_NE(clear, nullptr);
    EXPECT_FALSE(clear->HasClass("hidden")) << "a missing reference can be cleared";
}

TEST(ReferenceFieldTests, TheSameGuidSetAgainShowsMissingAfterTheAssetIsRemoved)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID crate = RegisterPrefab(registry, "Keg");

    AssetField field;
    field.SetAssetRegistry(&registry);
    field.SetValue(crate);
    ASSERT_EQ(NameLabel(field).GetText(), "Keg");

    registry.UnregisterAsset(crate);
    field.SetValue(crate);

    EXPECT_EQ(NameLabel(field).GetText(), "Missing asset");
    EXPECT_TRUE(NameLabel(field).HasClass("missing"));
}

TEST(ReferenceFieldTests, BothFieldsOfferAPickerButtonAfterTheClearButton)
{
    AssetField asset;
    EntityField entity;
    for (UIElement* field : {static_cast<UIElement*>(&asset), static_cast<UIElement*>(&entity)})
    {
        const auto& children = field->GetChildren();
        ASSERT_GE(children.size(), 2u);
        EXPECT_TRUE(children[children.size() - 2]->HasClass("asset-field-clear"));
        EXPECT_TRUE(children.back()->HasClass("asset-field-pick"));
        EXPECT_FALSE(children.back()->GetTooltip().empty());
    }
}

namespace
{

void MouseUpOn(UIElement& field, UIElement& target)
{
    UIEvent e{};
    e.Id = kEventMouseUp;
    e.Button = 0;
    e.Target = &target;
    e.CurrentTarget = &field;
    field.DispatchEvent(e);
}

} // namespace

TEST(ReferenceFieldTests, AClickOnARevealableFieldRevealsTheAssetAndTheNameReadsAsALink)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID crate = RegisterPrefab(registry, "Pallet");

    AssetField field;
    field.SetAssetRegistry(&registry);
    field.SetValue(crate);
    EXPECT_FALSE(NameLabel(field).HasClass("link")) << "no reveal action yet";

    std::filesystem::path revealed;
    field.SetOnReveal([&revealed](const std::filesystem::path& path) { revealed = path; });
    EXPECT_TRUE(NameLabel(field).HasClass("link"));

    MouseUpOn(field, NameLabel(field));
    EXPECT_TRUE(ContainsIgnoreCase(revealed.generic_string(), "Prefabs/Pallet.prefab"));
}

TEST(ReferenceFieldTests, AClickOnTheClearButtonClearsAndRevealsNothing)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();
    const GUID crate = RegisterPrefab(registry, "Cart");

    AssetField field;
    field.SetAssetRegistry(&registry);
    field.SetValue(crate);
    int reveals = 0;
    GUID changedTo = crate;
    field.SetOnReveal([&reveals](const std::filesystem::path&) { ++reveals; });
    field.SetOnValueChanged([&changedTo](const GUID& value) { changedTo = value; });

    UIElement* clear = ChildWithClass(field, "asset-field-clear");
    ASSERT_NE(clear, nullptr);
    MouseUpOn(*clear, *clear);
    EXPECT_TRUE(changedTo.IsNull());
    EXPECT_EQ(reveals, 0);
    EXPECT_EQ(NameLabel(field).GetText(), "None");
    EXPECT_FALSE(NameLabel(field).HasClass("link")) << "an empty field has nothing to reveal";
}

TEST(ReferenceFieldTests, AMissingAssetIsNotRevealable)
{
    AssetRegistry& registry = StartedEngine().GetAssetManager().GetRegistry();

    AssetField field;
    field.SetAssetRegistry(&registry);
    int reveals = 0;
    field.SetOnReveal([&reveals](const std::filesystem::path&) { ++reveals; });
    field.SetValue(GUID::Generate());

    EXPECT_FALSE(NameLabel(field).HasClass("link"));
    MouseUpOn(field, NameLabel(field));
    EXPECT_EQ(reveals, 0);
}
