// The Mark-ups panel header's view toggle (Mark-ups / Activity) is sized and styled as the Show all
// and Hide all buttons in the same row: the tabs carry every class those buttons carry. The chosen
// tab's highlight is ShippedMarkupsTabTests (UIStyleTests), over the shipped sheets.
//
// Source-level: the panel's layout loads only under a UI manager with the editor's sheets mounted,
// so the layout is read from the file itself.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace
{

std::string ReadEditorFile(const char* relativePath)
{
    std::ifstream in(std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The class attribute of the element whose id is `id` in `layout`, as written.
std::string ClassOf(const std::string& layout, const std::string& id)
{
    const std::regex element("id=\"" + id + "\"\\s+class=\"([^\"]*)\"");
    std::smatch match;
    return std::regex_search(layout, match, element) ? match[1].str() : std::string();
}

bool HasClass(const std::string& classes, const std::string& name)
{
    std::istringstream words(classes);
    for (std::string word; words >> word;)
    {
        if (word == name)
            return true;
    }
    return false;
}

} // namespace

TEST(MarkupsPanelHeaderTests, TheViewToggleIsTheSmallSecondaryButtonOfShowAll)
{
    const std::string layout = ReadEditorFile("Assets/UI/panels/MarkupsPanel.uxml");
    ASSERT_FALSE(layout.empty()) << "MarkupsPanel.uxml did not read";
    const std::string showAll = ClassOf(layout, "MarkupsShowAll");
    ASSERT_FALSE(showAll.empty()) << "MarkupsShowAll declares no class";
    EXPECT_EQ(showAll, ClassOf(layout, "MarkupsHideAll"));

    for (const char* tab : {"MarkupsTabMarkups", "MarkupsTabActivity"})
    {
        const std::string classes = ClassOf(layout, tab);
        std::istringstream words(showAll);
        for (std::string word; words >> word;)
            EXPECT_TRUE(HasClass(classes, word)) << tab << " lacks ." << word << " that Show all carries";
    }
}
