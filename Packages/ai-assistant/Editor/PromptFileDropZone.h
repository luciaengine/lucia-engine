#pragma once

#include "UI/Interaction/DropTarget.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <functional>
#include <span>

namespace GameEngine
{

/// A drop target for the prompt's files: the input row and the file strip above it. Files
/// dragged onto it, from the operating system's file browser or from the Assets panel, are
/// handed to the panel as if picked with the + button. While a drag from inside the editor is
/// over it, an allowed drop marks it `prompt-panel-drop-active` and tints its feedback element
/// (the + button) green; a refused one shows the prohibited icon there instead. Every payload
/// is previewed, so a drag it cannot take says so rather than falling through to the panel.
class PromptFileDropZone final : public UIElement, public UI::Interaction::IDropTarget
{
public:
    using FilesDropped = std::function<void(std::span<const std::filesystem::path>)>;

    void SetOnFilesDropped(FilesDropped onFilesDropped);
    /// The element whose icon shows whether the drag over the zone can be dropped.
    void SetFeedbackElement(UIElement* element);

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    /// Allowed when the drag holds at least one file; folders alone are refused.
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

private:
    FilesDropped m_OnFilesDropped;
    UIElement* m_FeedbackElement = nullptr;
};

} // namespace GameEngine
