#pragma once

#include <string>
#include <vector>

namespace GameEngine
{
class AssetRegistry;
class ParserRegistry;
struct AssetManifest;

/// The refusals for the ForestStand components of the manifest's scenes and prefabs, read from
/// their files with their parsers: an archive
/// or a tree model that does not ship, an archive that is not a .geforest file, an archive that
/// resolves outside its asset folder, and an archive that does not open. Each refusal names the
/// scene and the fix. Empty when every stand can draw in the packaged game.
std::vector<std::string> CheckForestStandContent(const AssetManifest& manifest, const AssetRegistry& registry,
                                                 const ParserRegistry& parsers);
} // namespace GameEngine
