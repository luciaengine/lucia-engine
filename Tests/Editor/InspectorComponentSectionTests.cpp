// InspectorUI::AddComponentSection remembers whether the user closed a section: the panel rebuilds
// on every selection change, undo and discrete edit, and a section that reopens on each rebuild is
// a section nobody can close. A caller that reacts to the toggle passes its own handler, which must
// run without costing the section its memory.

#include <gtest/gtest.h>

#include "Inspectors/InspectorComponent/InspectorComponentSection.h"
#include "UI/Controls/Foldout.h"
#include "UI/UIElement.h"

#include <string>

using GameEngine::Foldout;
using GameEngine::UIElement;

TEST(InspectorComponentSection, ACollapsedSectionStaysCollapsedAcrossARebuild)
{
    constexpr const char* kKey = "InspectorComponentSectionTests/Collapse";
    {
        UIElement root;
        Foldout* section = GameEngine::InspectorUI::AddComponentSection(&root, kKey, "Section", true);
        ASSERT_NE(section, nullptr);
        EXPECT_TRUE(section->IsExpanded()) << "a new section opens at its default";
        section->SetExpanded(false);
    }

    UIElement rebuilt;
    Foldout* section = GameEngine::InspectorUI::AddComponentSection(&rebuilt, kKey, "Section", true);
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->IsExpanded()) << "the rebuild reopened a section the user closed";
}
