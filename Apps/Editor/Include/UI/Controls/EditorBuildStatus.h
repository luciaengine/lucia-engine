#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{
class Label;

/// The status bar's left corner: the build configuration with the executable's time, then the
/// editor version and commit. Both texts are set at construction; the layout is
/// UI/controls/EditorBuildStatus/EditorBuildStatus.uxml.
class EditorBuildStatus final : public UIElement
{
  public:
    EditorBuildStatus();
};
} // namespace GameEngine
