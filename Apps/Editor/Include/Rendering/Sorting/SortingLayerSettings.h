#pragma once

#include <filesystem>
#include <string>

namespace GameEngine::Rendering { struct SortingLayers; }
namespace GameEngine::Editor
{
/// Registers project-owned sorting layer names, stable IDs, and draw priorities.
void RegisterSortingLayerSettings();
/// Validates and atomically saves layer definitions before publishing them to the renderer.
bool SaveSortingLayerSettings(const std::filesystem::path& projectRoot,
    const Rendering::SortingLayers& layers, std::string& error);
}
