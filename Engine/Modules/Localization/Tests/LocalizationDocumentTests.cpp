#include "Localization/LocalizationDocument.h"
#include "Localization/MessageCheck.h"
#include <gtest/gtest.h>
#include <sstream>
using namespace GameEngine;
using namespace GameEngine::Localization;
namespace
{
LocalizationDocument Fixture()
{
    LocalizationDocument document;
    std::istringstream input(R"({"en":{"play":"Play","coins":"{count, plural, one{# coin} other{# coins}}"},"nl":{"play":"Spelen"},"$metadata":{"play":{"context":"Title screen","notes":"Imperative, not a noun"}}})");
    std::string error;
    EXPECT_TRUE(document.Load(input, error)) << error;
    return document;
}
std::string Bytes(const LocalizationDocument& document)
{
    std::ostringstream output; std::string error;
    EXPECT_TRUE(document.Write(output, error)) << error;
    return output.str();
}
}
TEST(LocalizationDocument, MetadataAndMissingSurviveRoundTrip)
{
    auto document = Fixture();
    EXPECT_EQ(document.Missing("nl"), std::vector<StringId>{HashStringId("coins")});
    std::istringstream saved(Bytes(document));
    LocalizationDocument restored; std::string error;
    ASSERT_TRUE(restored.Load(saved, error)) << error;
    EXPECT_EQ(restored.Entries().at(HashStringId("play")).Context, "Title screen");
    EXPECT_EQ(restored.Entries().at(HashStringId("play")).Notes, "Imperative, not a noun");
    EXPECT_EQ(Bytes(restored), Bytes(document));
}
TEST(LocalizationDocument, InvalidEditsAreTransactional)
{
    auto document = Fixture(); const auto before = Bytes(document);
    auto entry = document.Entries().at(HashStringId("coins"));
    entry.Translations["nl"] = "{renamed}";
    std::string error;
    EXPECT_FALSE(document.Update(entry, error));
    EXPECT_EQ(Bytes(document), before);
    std::istringstream malformed(R"({"en":{"play":"x","play":"y"}})");
    EXPECT_FALSE(document.Load(malformed, error));
    EXPECT_EQ(Bytes(document), before);
}
TEST(LocalizationDocument, LocaleNormalizationAndIndependentPreview)
{
    auto document = Fixture(); std::string error;
    ASSERT_TRUE(document.AddLocale("pt_BR", error));
    EXPECT_EQ(document.Locales().back(), "pt-BR");
    EXPECT_EQ(document.Missing("pt-BR").size(), 2u);
    const auto english = document.CreatePreview("en", error);
    const auto dutch = document.CreatePreview("nl", error);
    ASSERT_TRUE(english && dutch);
    EXPECT_EQ(english->Resolve(HashStringId("play"), ""), "Play");
    EXPECT_EQ(dutch->Resolve(HashStringId("play"), ""), "Spelen");
    EXPECT_EQ(dutch->Resolve(HashStringId("coins"), "", {{"count", int64_t{2}}}), "2 coins");
}
TEST(LocalizationDocument, FindKeysFiltersCaseInsensitivelyAndByCoverage)
{
    auto document = Fixture();
    auto entry = document.Entries().at(HashStringId("play"));
    entry.Key = "Menu.Quit"; entry.Translations = {{"en", "Quit"}};
    std::string error;
    ASSERT_TRUE(document.Update(entry, error)) << error;
    const auto key = [&](StringId id) { return document.Entries().at(id).Key; };
    const auto all = document.FindKeys("", "nl", false);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(key(all[0]), "coins");
    EXPECT_EQ(key(all[1]), "Menu.Quit");
    EXPECT_EQ(key(all[2]), "play");
    const auto menu = document.FindKeys("menu", "nl", false);
    ASSERT_EQ(menu.size(), 1u);
    EXPECT_EQ(key(menu[0]), "Menu.Quit");
    const auto missing = document.FindKeys("", "nl", true);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(key(missing[0]), "coins");
    EXPECT_EQ(key(missing[1]), "Menu.Quit");
    EXPECT_TRUE(document.FindKeys("PLAY", "nl", true).empty());
}
TEST(LocalizationPo, RoundTripPreservesIcuAndNotes)
{
    auto document = Fixture(); std::string error;
    auto entry = document.Entries().at(HashStringId("coins"));
    entry.Translations["nl"] = "{count, plural, one{# munt} other{# munten}}";
    ASSERT_TRUE(document.Update(entry, error)) << error;
    std::ostringstream output;
    ASSERT_TRUE(document.ExportPo(output, "nl", error));
    EXPECT_NE(output.str().find("#. Context: Title screen"), std::string::npos);
    auto imported = Fixture(); std::istringstream input(output.str());
    ASSERT_TRUE(imported.ImportPo(input, "nl", error)) << error;
    EXPECT_EQ(Bytes(imported), Bytes(document));
    EXPECT_TRUE(imported.Missing("nl").empty());
}
TEST(LocalizationPo, RejectsStaleSourceDuplicateAndBadPlaceholdersWithoutPartialImport)
{
    for (const auto& po : {
        "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"Nieuw\"\n\nmsgctxt \"coins\"\nmsgid \"old\"\nmsgstr \"x\"\n",
        "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"a\"\n\nmsgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"b\"\n",
        "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"{bad}\"\n"})
    {
        auto document = Fixture(); const auto before = Bytes(document); std::string error;
        std::istringstream input(po);
        EXPECT_FALSE(document.ImportPo(input, "nl", error));
        EXPECT_FALSE(error.empty()); EXPECT_EQ(Bytes(document), before);
    }
}
TEST(LocalizationPo, MultilineFuzzyAndEmptyEntries)
{
    auto document = Fixture(); std::string error;
    std::istringstream multiline("msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"Spe\"\n\"len!\"\n");
    ASSERT_TRUE(document.ImportPo(multiline, "nl", error)) << error;
    EXPECT_EQ(document.Entries().at(HashStringId("play")).Translations.at("nl"), "Spelen!");
    const auto before = Bytes(document);
    for (const auto& po : {"#, fuzzy\nmsgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"wrong\"\n", "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"\"\n"})
    {
        std::istringstream input(po);
        EXPECT_TRUE(document.ImportPo(input, "nl", error)) << error;
        EXPECT_EQ(Bytes(document), before);
    }
}
TEST(LocalizationPo, RejectsWrongLanguagePluralArraysAndMalformedInput)
{
    for (const auto& po : {"msgid \"\"\nmsgstr \"Language: de\\n\"\n", "msgid_plural \"x\"\n", "msgstr[0] \"x\"\n", "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"bad\\q\"\n", "msgctxt \"play\"", ""})
    {
        auto document = Fixture(); const auto before = Bytes(document); std::string error;
        std::istringstream input(po);
        EXPECT_FALSE(document.ImportPo(input, "nl", error));
        EXPECT_EQ(Bytes(document), before);
    }
}
TEST(LocalizationPreview, PseudolocalizationLeavesCatalogUntouched)
{
    auto document = Fixture(); const auto before = Bytes(document); std::string error;
    auto preview = document.CreatePreview("en", error);
    const auto text = preview->Resolve(HashStringId("coins"), "", {{"count", int64_t{2}}});
    EXPECT_EQ(Pseudolocalize(text), "[!! 2 cöïns~~~ !!]");
    EXPECT_EQ(Pseudolocalize("日本語"), "[!! 日本語~ !!]");
    EXPECT_EQ(Bytes(document), before);
}
TEST(LocalizationPo, EscapedUnicodeAndControlCharactersRoundTrip)
{
    auto document = Fixture(); std::string error;
    auto entry = document.Entries().at(HashStringId("play"));
    entry.Translations["en"] = "Line\n\"quote\"\\tab\tend\f";
    entry.Translations["nl"] = "日本語\n\"tekst\"\\tab\tend\f";
    ASSERT_TRUE(document.Update(entry, error)) << error;
    std::ostringstream exported;
    ASSERT_TRUE(document.ExportPo(exported, "nl", error));
    std::istringstream input(exported.str());
    ASSERT_TRUE(document.ImportPo(input, "nl", error)) << error;
    EXPECT_EQ(document.Entries().at(HashStringId("play")).Translations.at("nl"), entry.Translations.at("nl"));
}
TEST(LocalizationDocument, RejectsMalformedMetadataWithoutReplacingDocument)
{
    for (const auto& data : {R"({"en":{"x":"X"},"$metadata":{"x":{"notes":2}}})", R"({"en":{"x":"X"},"$metadata":{"absent":{"context":"bad"}}})"})
    {
        auto document = Fixture(); const auto before = Bytes(document); std::string error;
        std::istringstream input(data);
        EXPECT_FALSE(document.Load(input, error));
        EXPECT_EQ(Bytes(document), before);
    }
}
TEST(LocalizationDocument, RejectsUnregisteredLocaleInEntry)
{
    auto document = Fixture(); const auto before = Bytes(document); std::string error;
    auto entry = document.Entries().at(HashStringId("play"));
    entry.Translations["de"] = "Spielen";
    EXPECT_FALSE(document.Update(entry, error));
    EXPECT_EQ(Bytes(document), before);
    ASSERT_TRUE(document.AddLocale("de", error));
    EXPECT_TRUE(document.Update(entry, error));
}
TEST(LocalizationPo, AdjacentEntriesDoNotRequireBlankLines)
{
    auto document = Fixture(); std::string error;
    std::istringstream input("msgid \"\"\nmsgstr \"Language: nl\\nContent-Type: text/plain; charset=UTF-8\\n\"\n#. A note\nmsgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"Spelen!\"\n");
    ASSERT_TRUE(document.ImportPo(input, "nl", error)) << error;
    EXPECT_EQ(document.Entries().at(HashStringId("play")).Translations.at("nl"), "Spelen!");
}
TEST(LocalizationMessageCheck, ValidPatternsHaveNoProblem)
{
    EXPECT_FALSE(CheckMessage("Play", "en"));
    EXPECT_FALSE(CheckMessage("{count, plural, one {# coin} other {# coins}}", "en"));
    EXPECT_FALSE(CheckMessage("'{' is a brace", "en"));
}
TEST(LocalizationMessageCheck, UnclosedBraceNamesItsCharacter)
{
    const auto problem = CheckMessage("Hi {count, plural, one {# coin}", "en");
    ASSERT_TRUE(problem);
    EXPECT_EQ(problem->Character, 4);
    EXPECT_EQ(problem->Problem, "a { is never closed");
    // Closed, this text still has no other case, so the fix shows no example that would fail too.
    EXPECT_EQ(problem->Fix, "Close it with } where the argument ends.");
    // The fix shows the author's own text closed when that closed text is valid.
    const auto name = CheckMessage("Hello {name", "en");
    ASSERT_TRUE(name);
    EXPECT_EQ(name->Fix, "Close it with }, e.g. Hello {name}");
    const auto plural = CheckMessage("Hi {count, plural, one {# coin} other {# coins}", "en");
    ASSERT_TRUE(plural);
    EXPECT_EQ(plural->Fix, "Close it with }, e.g. Hi {count, plural, one {# coin} other {# coins}}");
}
TEST(LocalizationMessageCheck, LiteralClosingBraceAndMissingOtherCase)
{
    // ICU treats a closing brace outside an argument as literal text.
    LocalizationCatalog catalog;
    std::istringstream literal(R"({"en":{"m":"Play}"}})");
    std::string error;
    ASSERT_TRUE(catalog.Load(literal, error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("m"), "fallback"), "Play}");
    EXPECT_FALSE(CheckMessage("Play}", "en"));
    const auto other = CheckMessage("{count, plural, one {# coin}}", "en");
    ASSERT_TRUE(other);
    EXPECT_EQ(other->Problem, "the plural or select has no other {...} case");
}
TEST(LocalizationMessageCheck, ArgumentNamesAreSortedAndUnique)
{
    EXPECT_EQ(MessageArgumentNames("{name} has {count, plural, one {# coin} other {# coins}} {name}"),
              (std::vector<std::string>{"count", "name"}));
    EXPECT_TRUE(MessageArgumentNames("{broken").empty());
}
TEST(LocalizationMessageCheck, MissingPluralCategoriesFollowTheLanguage)
{
    const std::string polish = "{count, plural, one {# moneta} few {# monety} many {# monet} other {# monety}}";
    EXPECT_TRUE(MissingPluralCategories(polish, "pl").empty());
    EXPECT_EQ(MissingPluralCategories("{count, plural, =1 {# moneta} other {# monety}}", "pl"),
              (std::vector<std::string>{"few", "many", "one"}));
    EXPECT_TRUE(MissingPluralCategories("{count, plural, other {# 枚}}", "ja").empty());
    EXPECT_EQ(MissingPluralCategories("{a, plural, one {{b, plural, other {x}}} other {y}}", "en"),
              (std::vector<std::string>{"one"}));
    EXPECT_TRUE(MissingPluralCategories("Play", "pl").empty());
    EXPECT_TRUE(MissingPluralCategories("{count, plural, one {x}", "pl").empty());
}
TEST(LocalizationDocument, PluralWithoutTheLanguageCategoriesCountsAsMissing)
{
    auto document = Fixture();
    auto entry = document.Entries().at(HashStringId("coins"));
    entry.Translations["nl"] = "{count, plural, other {# munten}}";
    std::string error;
    ASSERT_TRUE(document.Update(entry, error)) << error;
    EXPECT_EQ(document.Missing("nl"), std::vector<StringId>{HashStringId("coins")});
    EXPECT_EQ(document.FindKeys("", "nl", true), std::vector<StringId>{HashStringId("coins")});
    entry.Translations["nl"] = "{count, plural, one {# munt} other {# munten}}";
    ASSERT_TRUE(document.Update(entry, error)) << error;
    EXPECT_TRUE(document.Missing("nl").empty());
}
TEST(LocalizationPo, KeyErrorsNameTheEntryLine)
{
    auto document = Fixture(); std::string error;
    std::istringstream input("msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"Spelen\"\n\n"
                             "msgctxt \"unknown\"\nmsgid \"Gone\"\nmsgstr \"Weg\"\n\n"
                             "msgctxt \"coins\"\nmsgid \"{count, plural, one{# coin} other{# coins}}\"\nmsgstr \"\"\n");
    EXPECT_FALSE(document.ImportPo(input, "nl", error));
    EXPECT_EQ(error.rfind("PO near line 5: ", 0), 0u) << error;
}

TEST(LocalizationMessageCheck, UnfinishedQuotedArgumentHasABoundedDiagnostic)
{
    const auto problem = CheckMessage("Hello {name '{unfinished", "en");
    ASSERT_TRUE(problem);
    EXPECT_FALSE(problem->Fix.empty());
}
TEST(LocalizationMessageCheck, QuotedPluralPoundAndClosingBraceAreValid)
{
    const std::string pattern = "{count, plural, other {'# }'}}";
    LocalizationCatalog catalog;
    std::string error;
    std::istringstream source(R"({"en":{"m":"{count, plural, other {'# }'}}"}})");
    ASSERT_TRUE(catalog.Load(source, error)) << error;
    EXPECT_EQ(catalog.Resolve(HashStringId("m"), "fallback", {{"count", int64_t{1}}}), "# }");
    EXPECT_FALSE(CheckMessage(pattern, "en"));
}

TEST(LocalizationMessageCheck, BracePositionsCountUnicodeCodePoints)
{
    const auto problem = CheckMessage("\xf0\x9f\x8c\x8d {name", "en");
    ASSERT_TRUE(problem);
    EXPECT_EQ(problem->Character, 3);
}
