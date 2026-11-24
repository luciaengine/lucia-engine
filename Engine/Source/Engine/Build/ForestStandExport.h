#pragma once

#include <string>
#include <vector>

namespace GameEngine
{
struct AssetManifestEntry;

/// Checks the file and placement pages of an archive shipped without a scene reference.
void CheckShippedForestArchive(const AssetManifestEntry& entry, std::vector<std::string>& errors);

/// Registers the export check of the ForestStand fields of the shipped scenes and prefabs with
/// ContentCheckRegistry. It refuses an archive or tree model that does not ship, and an archive that
/// is not a .geforest file, links outside its asset folder or fails validation.
void RegisterForestStandContentCheck();
} // namespace GameEngine
