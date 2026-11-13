// The break above a registered settings row that starts a named group: the group's
// SectionHeader as a header line and its SectionDescription as the shared info card, so the
// global "Show Info Cards" switch hides the explanation and keeps the header.

#include "Editor/Settings/SettingsSectionBreak.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/InfoCard.h"

#include "../Editor/ScopedEditorUserDataRoot.h"
#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// Sets the global "Show Info Cards" switch for one test. The switch saves to the editor
// preferences, so the preferences point at an empty directory for the test's duration.
class ScopedInfoCardsVisible
{
public:
    explicit ScopedInfoCardsVisible(bool visible)
    {
        Editor::InfoCardAppearanceSettings& settings = Editor::InfoCardAppearanceSettings::Get();
        m_WasVisible = settings.GetCardsVisible();
        settings.SetCardsVisible(visible);
    }

    ~ScopedInfoCardsVisible()
    {
        Editor::InfoCardAppearanceSettings::Get().SetCardsVisible(m_WasVisible);
    }

    ScopedInfoCardsVisible(const ScopedInfoCardsVisible&) = delete;
    ScopedInfoCardsVisible& operator=(const ScopedInfoCardsVisible&) = delete;

private:
    TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("SettingsSectionBreakTests")};
    ScopedEditorUserDataRoot m_UserDataRoot{m_Root.Path()};
    bool m_WasVisible = true;
};

Editor::SettingsFieldDescriptor MakeGroupStart(std::string header, std::string description)
{
    Editor::SettingsFieldDescriptor field;
    field.Label = "Window mode";
    field.SectionHeader = std::move(header);
    field.SectionDescription = std::move(description);
    return field;
}

// The section break's parts: the children of the body its layout gives it.
const UIElement& BodyOf(const Editor::SettingsSectionBreak& sectionBreak)
{
    EXPECT_EQ(sectionBreak.GetChildren().size(), 1u) << "the layout did not load";
    if (sectionBreak.GetChildren().empty())
        return sectionBreak;
    return *sectionBreak.GetChildren()[0];
}

std::vector<const EditorUI::CollapsibleInfoCard*> CardsIn(const UIElement& element)
{
    std::vector<const EditorUI::CollapsibleInfoCard*> cards;
    for (const auto& child : element.GetChildren())
        if (const auto* card = dynamic_cast<const EditorUI::CollapsibleInfoCard*>(child.get()))
            cards.push_back(card);
    return cards;
}

const Label* HeaderIn(const UIElement& element)
{
    for (const auto& child : element.GetChildren())
        if (const auto* label = dynamic_cast<const Label*>(child.get()))
            if (label->HasClass("settings-subsection-header"))
                return label;
    return nullptr;
}

} // namespace

TEST(SettingsSectionBreak, TheDescriptionIsTheSharedInfoCardUnderTheHeader)
{
    ScopedInfoCardsVisible cardsOn(true);
    const std::string description = "Default window mode for exported Player builds (written to game.config).";
    const Editor::SettingsSectionBreak sectionBreak(MakeGroupStart("Player", description));
    const UIElement& body = BodyOf(sectionBreak);

    ASSERT_EQ(body.GetChildren().size(), 2u);
    const Label* header = HeaderIn(body);
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->GetText(), "Player");
    EXPECT_FALSE(header->HasClass("hidden"));
    EXPECT_EQ(body.GetChildren()[0].get(), header) << "the header comes before the card";

    const std::vector<const EditorUI::CollapsibleInfoCard*> cards = CardsIn(body);
    ASSERT_EQ(cards.size(), 1u) << "the description is not an info card";
    EXPECT_EQ(cards[0]->GetFullText(), description);
    EXPECT_TRUE(cards[0]->HasClass("editor-info-card"));
    EXPECT_FALSE(cards[0]->HasClass("hidden"));
    EXPECT_FALSE(cards[0]->HasClass(EditorUI::kInfoCardOffClass));
}

TEST(SettingsSectionBreak, ShowInfoCardsOffHidesTheDescriptionAndKeepsTheHeader)
{
    ScopedInfoCardsVisible cardsOff(false);
    const Editor::SettingsSectionBreak sectionBreak(
        MakeGroupStart("Render pipeline", "Pipeline written to game.config for builds."));
    const UIElement& body = BodyOf(sectionBreak);

    const Label* header = HeaderIn(body);
    ASSERT_NE(header, nullptr);
    EXPECT_FALSE(header->HasClass(EditorUI::kInfoCardOffClass));

    const std::vector<const EditorUI::CollapsibleInfoCard*> cards = CardsIn(body);
    ASSERT_EQ(cards.size(), 1u) << "the description is not an info card";
    EXPECT_TRUE(cards[0]->HasClass(EditorUI::kInfoCardOffClass)) << "the switch does not hide the description";
}

TEST(SettingsSectionBreak, AGroupWithoutADescriptionShowsNoCard)
{
    ScopedInfoCardsVisible cardsOn(true);
    const Editor::SettingsSectionBreak sectionBreak(MakeGroupStart("Steam Deck install", ""));
    const UIElement& body = BodyOf(sectionBreak);

    const Label* header = HeaderIn(body);
    ASSERT_NE(header, nullptr);
    EXPECT_FALSE(header->HasClass("hidden"));
    const std::vector<const EditorUI::CollapsibleInfoCard*> cards = CardsIn(body);
    ASSERT_EQ(cards.size(), 1u);
    EXPECT_TRUE(cards[0]->HasClass("hidden")) << "an empty description shows an empty card";
}
