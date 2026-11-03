// Editor windows blend text in the display-encoded space; a UIManager left at
// its default keeps the linear target.

#include <gtest/gtest.h>

#include "Editor/Settings/EditorTextBlend.h"
#include "UI/UIManager.h"

TEST(EditorTextBlend, EditorWindowsTargetTheEncodedSpaceAndTheDefaultStaysLinear)
{
    GameEngine::UIManager ui(nullptr);
    EXPECT_FLOAT_EQ(ui.GetTextBlendGamma(), 0.0f) << "a UIManager's default target must stay linear";
    GameEngine::Editor::ApplyEditorTextBlend(ui);
    EXPECT_FLOAT_EQ(ui.GetTextBlendGamma(), 1.0f);
}
