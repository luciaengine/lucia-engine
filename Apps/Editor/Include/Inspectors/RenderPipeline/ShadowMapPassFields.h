#pragma once

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace GameEngine
{
class UIElement;

/// Writes one field of a pass and saves the pipeline as one undo step named `undoLabel`.
using PipelinePassFieldCommit =
    std::function<void(const std::string& key, nlohmann::json value, const std::string& undoLabel)>;

/// Adds the rows of a ShadowMap pass to its section in the render pipeline inspector: the cascade
/// resolution, the cascade count, the split lambda, the scene distance fit and the max shadow distance.
/// A row shows the node default when the pass does not set the key. A resolution the node refuses stays
/// selected and is marked as invalid, so the inspector does not change the file on its own.
/// @param idPrefix the prefix of the row ids, for example "rp-pass-3"
void BuildShadowMapPassFields(UIElement& passSection, const nlohmann::json& pass, const std::string& idPrefix,
                              PipelinePassFieldCommit commit);

} // namespace GameEngine
