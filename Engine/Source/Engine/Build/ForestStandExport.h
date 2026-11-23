#pragma once

#include <string>
#include <vector>

namespace GameEngine
{
class AssetCollector;
struct AssetManifest;

/// The refusals for the ForestStand fields of the manifest's scenes and prefabs, from the edges that
/// the collector read: an archive or tree model that does not ship, and an archive that is not a
/// .geforest file, links outside its asset folder or does not open. Empty when every stand can draw.
std::vector<std::string> CheckForestStandContent(const AssetManifest& manifest, const AssetCollector& collector);
} // namespace GameEngine
