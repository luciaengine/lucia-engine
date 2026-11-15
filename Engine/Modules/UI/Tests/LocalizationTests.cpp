#include "UI/Controls/Label.h"
#include "IsolatedUIFixture.h"
#include "UI/Registration/ElementRegistration.h"
#include "../Source/UIAttributeAccess.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace GameEngine;

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
