#include "Rendering/Sorting/SortingLayerSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/SortingLayers.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine::Editor
{
bool SaveSortingLayerSettings(const std::filesystem::path& projectRoot,
    const Rendering::SortingLayers& layers, std::string& error)
{
    if (projectRoot.empty())
    {
        error = "Open a project before editing sorting layers.";
        return false;
    }
    if (!layers.Validate(error))
        return false;
    auto store = OpenProjectSettings(projectRoot);
    if (!store.Load(&error))
        return false;
    // Deleted IDs remain reserved: a later layer must never capture old scene references.
    int64_t nextId = 1;
    store.TryGetInt64("sortingLayerNextId", nextId);
    for (const auto& layer : Rendering::SortingLayers::FromJson(store.Json()).Layers)
        nextId = std::max(nextId, static_cast<int64_t>(layer.Id) + 1);
    for (const auto& layer : layers.Layers)
        nextId = std::max(nextId, static_cast<int64_t>(layer.Id) + 1);
    layers.WriteToJson(store.Json());
    store.SetInt64("sortingLayerNextId", nextId);
    if (!store.Save(&error))
        return false;
    return Rendering::SortingLayers::SetCurrent(layers, error);
}
}
