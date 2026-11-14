#include "UI/Controls/Label.h"
#include "IsolatedUIFixture.h"
#include "UI/Registration/ElementRegistration.h"
#include "../Source/UIAttributeAccess.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace GameEngine;

TEST(LabelLocalizationTests, HiddenMenuWarmupResolvesNestedFontsWithoutShowingTheMenu)
{
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(1.0f,
        R"(<uielement id="root"><uielement id="menu"><uielement id="group"><label id="title">冒険</label></uielement></uielement></uielement>)",
        R"(#root { width: 400px; height: 300px; }
            #menu { display: none; font-family: Roboto; font-size: 18px; }
            #group { font-size: 24px; }
            .large { font-size: 32px; })");
    if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
    ASSERT_TRUE(built) << fixture.Diagnostic();
    auto* root = fixture.Element("root");
    auto* title = fixture.Element("title");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(title, nullptr);
    // Callers can preload a nested screen directly while its parent stays hidden.
    ASSERT_NE(fixture.Element("group"), nullptr);
    EXPECT_EQ(fixture.Manager().PrepareTextSubtree(fixture.Element("group")),
              UIManager::TextWarmupStatus::Ready);
    EXPECT_EQ(fixture.Manager().PrepareTextSubtree(root), UIManager::TextWarmupStatus::Ready);
    EXPECT_FLOAT_EQ(title->GetResolvedStyle().Visual.FontSize, 24.0f);
    EXPECT_EQ(fixture.Element("menu")->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None);

    title->AddClass("large");
    EXPECT_EQ(fixture.Manager().PrepareTextSubtree(root), UIManager::TextWarmupStatus::Ready);
    EXPECT_FLOAT_EQ(title->GetResolvedStyle().Visual.FontSize, 32.0f);
    EXPECT_EQ(fixture.Element("menu")->GetResolvedStyle().Layout.DisplayMode, DisplayMode::None);
}

TEST(LabelLocalizationTests, WarmupRejectsUnownedTrees)
{
    UIManager manager(nullptr);
    UIElement detached;
    EXPECT_EQ(manager.PrepareTextSubtree(nullptr), UIManager::TextWarmupStatus::Failed);
    EXPECT_EQ(manager.PrepareTextSubtree(&detached), UIManager::TextWarmupStatus::Failed);
}

TEST(LabelLocalizationTests, SharedCatalogRefreshesLabelsAndPluralArguments)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"play":"Play","coins":"{n, plural, one {# coin} other {# coins}}"},"nl":{"play":"Spelen","coins":"{n, plural, one {# munt} other {# munten}}"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error)) << error;
    UIManager manager(nullptr);
    manager.SetLocalizationCatalog(catalog);
    Label play;
    Label coins;
    play.SetOwnerManager(&manager);
    coins.SetOwnerManager(&manager);
    play.BindLocalization(HashStringId("play"), "Play");
    coins.BindLocalization(HashStringId("coins"), "Coins", {{"n", int64_t{1}}});
    EXPECT_EQ(play.GetText(), "Play");
    EXPECT_EQ(coins.GetText(), "1 coin");
    play.ClearDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    ASSERT_TRUE(catalog->SetLocale("nl", error));
    EXPECT_TRUE(play.IsDirty(UIElement::LayoutDirty));
    EXPECT_EQ(play.GetText(), "Spelen");
    EXPECT_EQ(coins.GetText(), "1 munt");
    coins.SetLocalizationArguments({{"n", int64_t{2}}});
    EXPECT_EQ(coins.GetText(), "2 munten");
}

TEST(LabelLocalizationTests, RebindingUnsubscribesAndDestructionIsSafe)
{
    auto first = std::make_shared<Localization::LocalizationCatalog>();
    auto second = std::make_shared<Localization::LocalizationCatalog>();
    std::string error;
    std::istringstream a(R"({"en":{"play":"First"},"nl":{"play":"Eerste"}})");
    std::istringstream b(R"({"en":{"play":"Second"}})");
    ASSERT_TRUE(first->Load(a, error));
    ASSERT_TRUE(second->Load(b, error));
    {
        UIManager manager(nullptr);
        manager.SetLocalizationCatalog(first);
        Label label;
        label.SetOwnerManager(&manager);
        label.BindLocalization( HashStringId("play"), "fallback");
        manager.SetLocalizationCatalog(second);
        ASSERT_TRUE(first->SetLocale("nl", error));
        EXPECT_EQ(label.GetText(), "Second");
        label.BindLocalization(0, "");
        label.SetText("Manual");
        ASSERT_TRUE(second->SetLocale("nl", error));
        EXPECT_EQ(label.GetText(), "Manual");
        label.BindLocalization(HashStringId("missing"), "Visible fallback");
        EXPECT_EQ(label.GetText(), "Visible fallback");
    }
    EXPECT_TRUE(second->SetLocale("en", error));
}

TEST(LabelLocalizationTests, AuthoredKeysUseSharedContextAndRestoreFallback)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"play":"Play"},"nl":{"play":"Spelen"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error));
    UIManager manager(nullptr);
    Label label;
    label.SetOwnerManager(&manager);
    static_cast<UIElement&>(label).OnAuthoredAttributesApplied({{"localization-key", "play"}, {"text", "Start"}});
    manager.SetLocalizationCatalog(catalog);
    EXPECT_EQ(label.GetText(), "Play");
    ASSERT_TRUE(catalog->SetLocale("nl", error));
    EXPECT_EQ(label.GetText(), "Spelen");
    manager.SetLocalizationCatalog(nullptr);
    EXPECT_EQ(label.GetText(), "Start");
    manager.SetLocalizationCatalog(catalog);
    static_cast<UIElement&>(label).OnAuthoredAttributesApplied({{"text", "Unbound"}});
    EXPECT_EQ(label.GetText(), "Unbound");
    ASSERT_TRUE(catalog->SetLocale("en", error));
    EXPECT_EQ(label.GetText(), "Unbound");
}

TEST(LabelLocalizationTests, BindingBeforeOwnershipAndMovingBetweenManagers)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"play":"Play"},"nl":{"play":"Spelen"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error));
    UIManager first(nullptr), second(nullptr);
    first.SetLocalizationCatalog(catalog);
    Label label;
    label.BindLocalization(HashStringId("play"), "Start");
    EXPECT_EQ(label.GetText(), "Start");
    label.SetOwnerManager(&first);
    EXPECT_EQ(label.GetText(), "Play");
    label.SetOwnerManager(&second);
    EXPECT_EQ(label.GetText(), "Start");
    ASSERT_TRUE(catalog->SetLocale("nl", error));
    EXPECT_EQ(label.GetText(), "Start");
    second.SetLocalizationCatalog(catalog);
    EXPECT_EQ(label.GetText(), "Spelen");
    label.SetOwnerManager(nullptr);
    EXPECT_EQ(label.GetText(), "Start");
    ASSERT_TRUE(catalog->SetLocale("en", error));
    EXPECT_EQ(label.GetText(), "Start");
}

TEST(LabelLocalizationTests, LiteralTextUnbindsEvenWhenItMatchesTheTranslation)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"play":"Play"},"nl":{"play":"Spelen"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error)) << error;
    UIManager manager(nullptr);
    manager.SetLocalizationCatalog(catalog);
    Label sameText, differentText;
    sameText.SetOwnerManager(&manager);
    differentText.SetOwnerManager(&manager);
    sameText.BindLocalization(HashStringId("play"), "Start");
    differentText.BindLocalization(HashStringId("play"), "Start");
    sameText.SetText("Play");
    differentText.SetText("Manual");
    ASSERT_TRUE(catalog->SetLocale("nl", error)) << error;
    EXPECT_EQ(sameText.GetText(), "Play");
    EXPECT_EQ(differentText.GetText(), "Manual");
    manager.SetLocalizationCatalog(nullptr);
    EXPECT_EQ(sameText.GetText(), "Play");
    EXPECT_EQ(differentText.GetText(), "Manual");
}

TEST(LabelLocalizationTests, RegisteredAuthoredTextPreservesPluralArgumentsUntilBindingIsRemoved)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"coins":"{n, plural, one {# coin} other {# coins}}"},"nl":{"coins":"{n, plural, one {# munt} other {# munten}}"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error)) << error;
    UIManager manager(nullptr);
    manager.SetLocalizationCatalog(catalog);
    auto& registry = UIRegistration::ElementFactoryRegistry::Instance();
    auto element = registry.Create("label");
    auto* label = dynamic_cast<Label*>(element.get());
    ASSERT_NE(label, nullptr);
    label->SetOwnerManager(&manager);
    UIAttributeAccess::SetAuthoredAttribute(*label, "localization-key", "coins");
    UIAttributeAccess::SetAuthoredAttribute(*label, "text", "Coins");
    registry.ApplyAttributes(*label, {{"localization-key", "coins"}, {"text", "Coins"}}, "");
    label->SetLocalizationArguments({{"n", int64_t{2}}});
    EXPECT_EQ(label->GetText(), "2 coins");
    registry.ApplyAttributes(*label, {{"localization-key", "coins"}, {"text", "Coins"}}, "");
    EXPECT_EQ(label->GetText(), "2 coins");
    // Inner text uses the other registered setter, before the final authored hook.
    registry.ApplyAttributes(*label, {{"localization-key", "coins"}}, "Coins");
    EXPECT_EQ(label->GetText(), "2 coins");
    ASSERT_TRUE(catalog->SetLocale("nl", error)) << error;
    EXPECT_EQ(label->GetText(), "2 munten");
    registry.ApplyAttributes(*label, {{"text", "Manual"}}, "");
    EXPECT_EQ(label->GetText(), "Manual");
    ASSERT_TRUE(catalog->SetLocale("en", error)) << error;
    EXPECT_EQ(label->GetText(), "Manual");
}

TEST(LabelLocalizationTests, ClearLocalizationKeepsDisplayedTranslation)
{
    auto catalog = std::make_shared<Localization::LocalizationCatalog>();
    std::istringstream input(R"({"en":{"play":"Play"},"nl":{"play":"Spelen"}})");
    std::string error;
    ASSERT_TRUE(catalog->Load(input, error)) << error;
    UIManager manager(nullptr);
    manager.SetLocalizationCatalog(catalog);
    Label label;
    label.SetOwnerManager(&manager);
    label.BindLocalization(HashStringId("play"), "Start");
    ASSERT_TRUE(catalog->SetLocale("nl", error)) << error;
    ASSERT_EQ(label.GetText(), "Spelen");
    label.ClearLocalization();
    EXPECT_EQ(label.GetText(), "Spelen");
    ASSERT_TRUE(catalog->SetLocale("en", error)) << error;
    manager.SetLocalizationCatalog(nullptr);
    EXPECT_EQ(label.GetText(), "Spelen");
    label.BindLocalization(0, label.GetText());
    EXPECT_EQ(label.GetText(), "Spelen");
}
