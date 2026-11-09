#pragma once

#include "AssetCore/GUID.h"

namespace GameEngine::Components
{

// Marks a placed prefab root by its asset GUID. SceneIO writes the prefab section from it.
// Other members are identified by their hierarchy and scene identity tags.

// @ge-no-add  placed by the scene loader, not user-addable in the editor
// [DoNotSerialize] SceneIO writes the [prefab] section from this marker; reflection would emit invalid property lines.
// @ge-engine-internal scene loading
struct PrefabInstance
{
    GUID Source{}; // @ge-hidden  the web types have no GUID kind, and no page reads the marker
};

static_assert(sizeof(PrefabInstance) == 16, "PrefabInstance is one GUID");

} // namespace GameEngine::Components
