#include "IsolatedUIFixture.h"
#include "Core/Application.h"
#include "UI/Controls/SearchFieldWithFilter.h"

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>

using namespace GameEngine;

TEST(PackageSearchLayout, ShippedSearchStaysCompactAndLeavesRoomForTheList)
{
    std::string css;
    for (const char* relative : {"theme/tokens.css", "theme/widgets.css", "theme/views.css",
                                 "panels/PackageManagerPanel.css"})
    {
        std::ifstream file(PathUtils::GetExecutableDirectory() / "Assets/UI" / relative);
        ASSERT_TRUE(file) << relative;
        std::ostringstream contents;
        contents << file.rdbuf();
        css += contents.str() + "\n";
    }
    for (int height : {180, 337, 548})
    {
        UITesting::IsolatedUIFixture fixture;
        const std::string geometry = "#panel { display: flex; flex-direction: column; width: 480px; height: " + std::to_string(height) + "px; }";
        if (!fixture.Build(1.0f, R"(<uielement><uielement id="panel" class="package-manager-panel">
            <uielement id="content" class="pkg-root"><uielement id="body" class="pkg-body"/></uielement>
            </uielement></uielement>)", css + geometry))
        {
            if (!fixture.DeviceAvailable()) GTEST_SKIP() << fixture.Diagnostic();
            FAIL() << fixture.Diagnostic();
        }
        auto search = std::make_unique<SearchFieldWithFilter>();
        search->AddClass("panel-search-bar");
        search->AddClass("pkg-search");
        search->SetPlaceholder("Search packages");
        auto* field = search.get();
        fixture.Element("content")->AddChild(std::move(search));
        fixture.Settle();
        EXPECT_GT(field->GetLayoutHeight(), 0.0f);
        EXPECT_LE(field->GetLayoutHeight(), 28.0f);
        EXPECT_GT(fixture.Element("body")->GetLayoutHeight(), height - 65.0f);
        bool checkedPlaceholder = false;
        for (const auto& child : field->GetChildren())
            if (child->HasClass("search-field-with-filter-placeholder"))
            {
                EXPECT_EQ(child->GetResolvedStyle().Visual.FontStyle, FontStyle::Normal);
                checkedPlaceholder = true;
            }
        EXPECT_TRUE(checkedPlaceholder);
        field->AddClass("search-bar-at-top");
        fixture.Settle();
        EXPECT_GE(fixture.Element("body")->GetLayoutY() -
            (field->GetLayoutY() + field->GetLayoutHeight()), 5.5f);
        // The owning panel must constrain the field even without the optional shared chrome.
        field->RemoveClass("panel-search-bar");
        fixture.Settle();
        EXPECT_LE(field->GetLayoutHeight(), 28.0f);
        EXPECT_GT(fixture.Element("body")->GetLayoutHeight(), height - 65.0f);
    }
}
