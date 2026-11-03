#include "PromptFileDropZone.h"

#include "Editor/DragDropPayloads.h"

#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
// The dragged paths that are files; folders are left out.
std::vector<std::filesystem::path> DroppedFiles(const UI::Interaction::DropRequest& request)
{
    std::vector<std::filesystem::path> files;
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload)
        return files;
    for (const std::filesystem::path& path : payload->paths)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            files.push_back(path);
    }
    return files;
}
} // namespace

void PromptFileDropZone::SetOnFilesDropped(FilesDropped onFilesDropped)
{
    m_OnFilesDropped = std::move(onFilesDropped);
}

void PromptFileDropZone::SetFeedbackElement(UIElement* element)
{
    m_FeedbackElement = element;
}

bool PromptFileDropZone::AcceptsPayload(UI::Interaction::PayloadTypeId) const
{
    return true;
}

bool PromptFileDropZone::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnItem;
    return true;
}

UI::Interaction::DropFeedback PromptFileDropZone::CanDrop(const UI::Interaction::DropRequest& request) const
{
    if (DroppedFiles(request).empty())
        return {false, "Drop files to add them to the prompt; folders cannot be added"};
    return {true, "Add to the prompt"};
}

void PromptFileDropZone::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const std::vector<std::filesystem::path> files = DroppedFiles(request);
    if (!files.empty() && m_OnFilesDropped)
        m_OnFilesDropped(files);
}

void PromptFileDropZone::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    const bool allowed = state.Visible && state.Allowed;
    const bool refused = state.Visible && !state.Allowed;
    if (allowed)
        AddClass("prompt-panel-drop-active");
    else
        RemoveClass("prompt-panel-drop-active");
    if (!m_FeedbackElement)
        return;
    if (allowed)
        m_FeedbackElement->AddClass("prompt-panel-drop-allowed-icon");
    else
        m_FeedbackElement->RemoveClass("prompt-panel-drop-allowed-icon");
    if (refused)
        m_FeedbackElement->AddClass("prompt-panel-drop-invalid-icon");
    else
        m_FeedbackElement->RemoveClass("prompt-panel-drop-invalid-icon");
}

} // namespace GameEngine
