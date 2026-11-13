#include "InspectorLayoutFixture.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

TEST(InspectorEntityHeaderLayout, HistoryButtonsShareOneRowAndKeepTheHeaderCompact)
{
    auto root = std::make_unique<GameEngine::UIElement>();
    auto header = std::make_unique<GameEngine::UIElement>();
    header->AddClass("inspector-header-container");
    GameEngine::UIElement* headerView = header.get();

    auto icon = std::make_unique<GameEngine::UIElement>();
    icon->AddClass("inspector-hierarchy-entity-icon");
    header->AddChild(std::move(icon));
    auto name = std::make_unique<GameEngine::Label>();
    name->AddClass("inspector-header");
    name->SetText("Crowd_0001");
    header->AddChild(std::move(name));

    GameEngine::UIRegistration::RegisterBuiltInControls();
    const std::string layout = InspectorLayoutTesting::ReadEditorFile(
        "Assets/UI/inspectors/EntityHeader/EntityHeader.uxml");
    ASSERT_FALSE(layout.empty());
    std::unique_ptr<GameEngine::UIElement> trailing;
    ASSERT_TRUE(GameEngine::UIParsing::XMLParser::ParseLayoutFromString(layout, trailing, "EntityHeader.uxml"));
    ASSERT_NE(trailing, nullptr);
    auto* back = dynamic_cast<GameEngine::Button*>(trailing->FindById(GameEngine::HashStringId("inspector-history-back")));
    auto* forward = dynamic_cast<GameEngine::Button*>(trailing->FindById(GameEngine::HashStringId("inspector-history-forward")));
    ASSERT_NE(back, nullptr);
    ASSERT_NE(forward, nullptr);
    header->AddChild(std::move(trailing));
    root->AddChild(std::move(header));

    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ExtraSheets.push_back("Assets/UI/controls/Button.css");
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    constexpr float kMinimumButtonHeight = 32.0f;
    constexpr float kMaximumHeaderHeight = 48.0f;
    constexpr float kLayoutTolerance = 1.0f / 64.0f;
    ASSERT_GE(back->GetLayoutHeight(), kMinimumButtonHeight);
    ASSERT_GE(forward->GetLayoutHeight(), kMinimumButtonHeight);
    EXPECT_NEAR(back->GetLayoutY(), forward->GetLayoutY(), kLayoutTolerance);
    EXPECT_GE(forward->GetLayoutX(), back->GetLayoutX() + back->GetLayoutWidth() - kLayoutTolerance);
    EXPECT_LE(headerView->GetLayoutHeight(), kMaximumHeaderHeight);
}
