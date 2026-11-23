#pragma once

namespace GameEngine
{
/// Registers the export check of the ForestStand fields of the shipped scenes and prefabs with
/// ContentCheckRegistry. It refuses an archive or tree model that does not ship, and an archive that
/// is not a .geforest file, links outside its asset folder or does not open.
void RegisterForestStandContentCheck();
} // namespace GameEngine
