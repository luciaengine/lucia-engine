#pragma once

#include "UI/UIElement.h"

#include <filesystem>

namespace GameEngine
{

/// A picked image's thumbnail in the prompt's file strip: the thumbnail service's downscaled
/// copy (EditorAssetActions::ShowImage), asked for while the element is in the panel and
/// evicted from the UI's textures when it leaves.
class PromptFileThumbnail final : public UIElement
{
public:
    explicit PromptFileThumbnail(std::filesystem::path image);

private:
    void RequestPicture();
    void ReleasePicture();

    std::filesystem::path m_Image;
};

} // namespace GameEngine
