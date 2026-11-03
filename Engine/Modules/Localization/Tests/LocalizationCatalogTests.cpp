#include "Localization/LocalizationAsset.h"
#include "Localization/LocalizationCatalog.h"
#include "Platform/Shell.h"
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>

using namespace GameEngine;
using GameEngine::Localization::LocalizationCatalog;

namespace
{
bool Load(LocalizationCatalog& catalog, const std::string& json, std::string& error)
{
    std::istringstream input(json);
    return catalog.Load(input, error);
}
}

TEST(LocalizationCatalog, AssetCreatesIndependentCatalogs)
{
    Localization::LocalizationAsset asset(GUID{}, "menu.localization");
    std::string error;
    EXPECT_FALSE(asset.CreateCatalog(error)) << "an unloaded asset produced a catalog";
    EXPECT_FALSE(error.empty());
    const std::string document = R"({"en":{"play":"Play"},"nl":{"play":"Spelen"}})";
    const Vector<uint8> bytes(document.begin(), document.end());
    ASSERT_TRUE(asset.LoadFromData(bytes));
    auto first = asset.CreateCatalog(error);
    auto second = asset.CreateCatalog(error);
    ASSERT_TRUE(first && second) << error;
    ASSERT_TRUE(first->SetLocale("nl", error)) << error;
    EXPECT_EQ(first->Resolve(HashStringId("play"), ""), "Spelen");
    EXPECT_EQ(second->Resolve(HashStringId("play"), ""), "Play") << "locale state leaked between catalogs";
    EXPECT_FALSE(asset.LoadFromData(Vector<uint8>{'{'})) << "a malformed asset was accepted";
    EXPECT_TRUE(asset.IsLoaded() && asset.CreateCatalog(error)) << "a failed load discarded the valid asset";
    asset.Unload();
    EXPECT_FALSE(asset.CreateCatalog(error)) << "the unloaded payload stayed available";
    EXPECT_EQ(first->Resolve(HashStringId("play"), ""), "Spelen") << "unloading the asset invalidated a catalog";
    EXPECT_EQ(AssetTypeFromString("Localization"), AssetType::Localization);
}

TEST(LocalizationCatalog, FallsBackThroughRegionAndEnglish)
{
    LocalizationCatalog catalog;
    std::string error;
    int changes = 0;
    auto subscription = catalog.Changed.Subscribe([&changes]() { ++changes; });
    ASSERT_TRUE(Load(catalog,
        R"({"en":{"play":"Play","quit":"Quit"},"nl":{"play":"Spelen"},"ja":{"play":"遊ぶ"},"pt-br":{"play":"Jogar"}})", error))
        << error;
    ASSERT_TRUE(catalog.SetLocale("NL_nl", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), "play"), "Spelen");
    EXPECT_EQ(catalog.Resolve(HashStringId("quit"), "quit"), "Quit");
    EXPECT_EQ(catalog.Resolve(HashStringId("missing"), "missing"), "missing");
    ASSERT_TRUE(catalog.SetLocale("ja", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), "play"), "遊ぶ");
    ASSERT_TRUE(catalog.SetLocale("PT_BR", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), "play"), "Jogar");
    ASSERT_TRUE(catalog.SetLocale("pt-br", error)) << error;
    EXPECT_EQ(changes, 4) << "setting the current locale again fired the event";

    EXPECT_FALSE(Load(catalog, R"({"en":{"play":42}})", error)) << "a non-string translation was accepted";
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), "play"), "Jogar") << "a failed load changed the catalog";
    EXPECT_EQ(changes, 4);
    EXPECT_FALSE(Load(catalog, R"({"en":{},"nl":{},"NL":{}})", error)) << "a duplicate normalized locale was accepted";
    EXPECT_FALSE(Load(catalog, R"({"nl":{}})", error)) << "a catalog without English was accepted";

    subscription.Reset();
    ASSERT_TRUE(catalog.SetLocale("zz", error)) << error;
    EXPECT_EQ(changes, 4) << "the event reached a reset subscription";
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), "play"), "Play");
}

TEST(LocalizationCatalog, InfersChineseScriptFromRegion)
{
    LocalizationCatalog catalog;
    std::string error;
    ASSERT_TRUE(Load(catalog,
        R"({"en":{"play":"Play"},"zh":{"play":"generic"},"zh-Hans":{"play":"简体"},"zh-Hant":{"play":"繁體"},"zh-HK":{"play":"香港"}})",
        error)) << error;
    ASSERT_TRUE(catalog.SetLocale("zh-TW", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), ""), "繁體");
    ASSERT_TRUE(catalog.SetLocale("zh-CN", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), ""), "简体");
    ASSERT_TRUE(catalog.SetLocale("zh-HK", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("play"), ""), "香港") << "the exact region lost to the inferred script";
}

TEST(LocalizationCatalog, FormatsPluralAndNamedArguments)
{
    LocalizationCatalog catalog;
    std::string error;
    ASSERT_TRUE(Load(catalog,
        R"({"en":{"coins":"{count, plural, one {# coin} other {# coins}}","hello":"Hello {name}"},"pl":{"coins":"{count, plural, one {# moneta} few {# monety} many {# monet} other {# monety}}"}})",
        error)) << error;
    ASSERT_TRUE(catalog.SetLocale("pl-PL", error)) << error;
    const auto coins = [&](int64_t count)
    { return catalog.Resolve(HashStringId("coins"), "fallback", {{"count", count}}, &error); };
    EXPECT_EQ(coins(1), "1 moneta");
    EXPECT_EQ(coins(2), "2 monety");
    EXPECT_EQ(coins(5), "5 monet");
    EXPECT_EQ(coins(22), "22 monety");
    EXPECT_EQ(catalog.Resolve(HashStringId("hello"), "fallback", {{"name", std::string("犬")}}, &error), "Hello 犬");
    error.clear();
    EXPECT_EQ(catalog.Resolve(HashStringId("coins"), "fallback", {}, &error), "fallback") << "a missing argument leaked message syntax";
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(catalog.SetLocale("en-!invalid", error));
    EXPECT_EQ(catalog.GetLocale(), "pl-PL") << "an invalid locale changed the state";
    EXPECT_FALSE(Load(catalog, R"({"en":{"coins":"{count, plural, one {coin}}"}})", error));
    EXPECT_EQ(coins(5), "5 monet") << "a malformed pattern replaced the active catalog";
}

TEST(LocalizationCatalog, RejectsRepeatedKeysAndRenamedArguments)
{
    LocalizationCatalog catalog;
    std::string error;
    EXPECT_FALSE(Load(catalog, R"({"en":{"play":"Play","play":"Duplicate"}})", error)) << "a repeated JSON key was accepted";
    EXPECT_FALSE(Load(catalog, R"({"en":{"x":"Hello {name}"},"nl":{"x":"Hallo {naam}"}})", error))
        << "a translation that renames an argument was accepted";
}

TEST(LocalizationCatalog, ResolvesNestedEmptyAndQuotedMessages)
{
    LocalizationCatalog catalog;
    std::string error;
    ASSERT_TRUE(Load(catalog,
        R"({"en":{"nested":"{kind, select, dog {{name} barks} other {{name} waits}}","empty":"","literal":"'{name}'"}})", error))
        << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("nested"), "fallback", {{"kind", std::string("dog")}}, &error), "fallback")
        << "a missing nested argument was accepted";
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(catalog.Resolve(HashStringId("nested"), "fallback",
                              {{"kind", std::string("dog")}, {"name", std::string("Dog")}}, &error), "Dog barks");
    EXPECT_TRUE(catalog.Resolve(HashStringId("empty"), "fallback").empty()) << "an intentional empty translation was lost";
    EXPECT_EQ(catalog.Resolve(HashStringId("literal"), "fallback"), "{name}") << "a quoted literal was read as an argument";
}

TEST(LocalizationCatalog, LocalePreferenceRoundTrips)
{
    LocalizationCatalog catalog;
    std::string error;
    ASSERT_TRUE(Load(catalog, R"({"en":{"play":"Play"}})", error)) << error;
    ASSERT_TRUE(catalog.SetLocale("ja-JP", error)) << error;
    std::ostringstream preference;
    ASSERT_TRUE(catalog.WriteLocale(preference, error)) << error;
    ASSERT_TRUE(catalog.SetLocale("en", error)) << error;
    std::istringstream saved(preference.str());
    EXPECT_TRUE(catalog.RestoreLocale(saved, error)) << error;
    EXPECT_EQ(catalog.GetLocale(), "ja-JP");
    std::istringstream invalidPreference(std::string(256, 'a'));
    EXPECT_FALSE(catalog.RestoreLocale(invalidPreference, error));
    EXPECT_EQ(catalog.GetLocale(), "ja-JP") << "an invalid preference changed the language";
}

// The example catalog, staged beside the suite under Localization/.
TEST(LocalizationCatalog, LoadsTheStagedExampleCatalog)
{
    const auto path = Platform::GetExecutablePath().parent_path() / "Localization" / "menu.localization";
    std::ifstream packaged(path, std::ios::binary);
    ASSERT_TRUE(packaged) << path << " is not staged beside the suite";
    LocalizationCatalog catalog;
    std::string error;
    ASSERT_TRUE(catalog.Load(packaged, error)) << error;
    ASSERT_TRUE(catalog.SetLocale("nl", error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("menu.coins"), "fallback", {{"count", int64_t{22}}}), "22 munten");
}

TEST(LocalizationCatalog, LanguageDisplayNamesAreEnglish)
{
    EXPECT_EQ(Localization::LanguageDisplayName("nl"), "Dutch");
    EXPECT_EQ(Localization::LanguageDisplayName("pt-BR"), "Portuguese (Brazil)");
    EXPECT_EQ(Localization::LanguageDisplayName("zh-Hant"), "Chinese (Traditional)");
}
