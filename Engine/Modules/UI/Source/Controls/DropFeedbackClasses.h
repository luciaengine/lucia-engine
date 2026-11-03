#pragma once

#include "UI/UIElement.h"

namespace GameEngine::UI {

/// Drop-target feedback classes shared by ListView, GridView and TreeView rows:
/// `drop-hover` marks the row under an OnItem drop, and `drop-allowed` or
/// `drop-denied` says whether the drop would be accepted.
inline void SetDropFeedbackClasses(UIElement& row, bool hovered, bool allowed)
{
    row.RemoveClass("drop-hover");
    row.RemoveClass("drop-allowed");
    row.RemoveClass("drop-denied");
    if (!hovered)
        return;
    row.AddClass("drop-hover");
    row.AddClass(allowed ? "drop-allowed" : "drop-denied");
}

} // namespace GameEngine::UI
