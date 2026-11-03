#pragma once

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h" // reflects the GUID field as FieldTypeId::AssetGuid
#include "Mathematics/Vector3.h"

namespace GameEngine::Components
{

// Marks an entity as the root of a scene [subscene] instance, with enough metadata
// to re-serialize the instance back to a scene file.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — SceneIO reconstructs the [subscene] section from this metadata; reflection would emit invalid property lines.
struct SceneSubsceneInstance
{
    char sourcePath[260]{};
    // The [resource] section's guid=, or null when the scene authored none.
    GUID sourceGuid{}; // @ge-readonly

    Mathematics::Vector3 offset{0.0f, 0.0f, 0.0f};
    bool hasOffset = false;

    // The path and guid buffers can be full with no terminator after a raw component write.
    [[nodiscard]] std::string_view SourcePath() const { return FixedStringView(sourcePath); }
    [[nodiscard]] const GUID& SourceGuid() const { return sourceGuid; }
};

} // namespace GameEngine::Components

