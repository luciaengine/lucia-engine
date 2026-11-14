#include "PromptFileThumbnail.h"

#include "Editor/Assets/EditorAssetActions.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine
{

PromptFileThumbnail::PromptFileThumbnail(std::filesystem::path image)
    : m_Image(std::move(image))
{
    AddClass("prompt-panel-file-preview");
    RegisterEventHandler(kEventAttachedToPanel, [this](UIEvent&) { RequestPicture(); });
    RegisterEventHandler(kEventDetachedFromPanel, [this](UIEvent&) { ReleasePicture(); });
}

void PromptFileThumbnail::RequestPicture()
{
    if (Style().Get(Style::BackgroundImage).has_value())
        return;
    if (const auto& show = Editor::GetEditorAssetActions().ShowThumbnail)
        show(*this, m_Image, 512);
}

void PromptFileThumbnail::ReleasePicture()
{
    const auto source = Style().Get(Style::BackgroundImage);
    if (!source)
        return;
    UIManager* ui = GetOwnerManager();
    if (ui && source->Kind == BackgroundImageSource::SourceKind::Path && !source->Value.empty())
        ui->BackgroundImages().Evict(source->Value);
    UI::Layout::DisableBackgroundOverride(*this);
}

} // namespace GameEngine
