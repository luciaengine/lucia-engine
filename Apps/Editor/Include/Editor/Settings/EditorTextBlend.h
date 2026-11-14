#pragma once

#include "UI/UIManager.h"

namespace GameEngine::Editor
{

/// The text blend target of an editor window's UI: the display-encoded space
/// (UIManager::SetTextBlendGamma). Editor windows draw into an encoded
/// attachment and their text matches Chrome, which blends text in that space,
/// so there the retarget is the identity. A UIManager left at its linear
/// default keeps area-exact ink, the identity on a linear attachment.
inline constexpr float kEditorTextBlendGamma = 1.0f;

/// Sets an editor window's UI to blend text in the display-encoded space.
inline void ApplyEditorTextBlend(UIManager& ui)
{
    ui.SetTextBlendGamma(kEditorTextBlendGamma);
}

} // namespace GameEngine::Editor
