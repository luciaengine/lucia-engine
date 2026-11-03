#pragma once

#include "PromptFiles.h"

#include "UI/UIElement.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>

namespace GameEngine
{
class Button;
class Label;
class UIManager;

/// One file in the prompt's file strip: the layout comes from the package's
/// `Editor/UI/controls/PromptFileItem.uxml` (the file's name and its x); the item fills it
/// and, for an image that has been read, adds its thumbnail before the name.
class PromptFileItem final : public UIElement
{
public:
    /// `index` is the item's position in the strip; it keys the item's element ids so every
    /// item's are unique. `onRemove` runs when the x is clicked.
    PromptFileItem(size_t index, std::function<void()> onRemove);

    /// Instantiates the item layout through `ui`. False (and logged) when the layout is not
    /// staged or is missing an element.
    bool Build(UIManager& ui);
    /// Shows the file at `path`: still being read while `file` is empty. Called once, after
    /// Build; the strip builds a new item when the file's state changes.
    void Show(const std::filesystem::path& path, const std::optional<PromptFile>& file);

private:
    size_t m_Index = 0;
    std::function<void()> m_OnRemove;
    Label* m_Label = nullptr;
    Button* m_Remove = nullptr;
};

} // namespace GameEngine
