// Empty definitions of the model thumbnail's animation preview for AnimationPanelReconcileTests:
// ModelThumbnailHandler.cpp brings the whole thumbnail renderer, and the tests preview no clip.

#include "Thumbnails/ModelThumbnail/ModelThumbnailHandler.h"

namespace GameEngine
{

void ModelThumbnailHandler::SetExternalAnimationPreview(const std::filesystem::path&,
                                                        const std::shared_ptr<Animation::AnimationClip>&, float)
{
}

void ModelThumbnailHandler::ClearExternalAnimationPreview()
{
}

} // namespace GameEngine
