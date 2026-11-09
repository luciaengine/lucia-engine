#include "TerrainECS/TerrainHeightSource.h"

namespace GameEngine::TerrainECS
{

bool TerrainHeightIsPaged(const TerrainHeightSourceInputs& inputs)
{
    if (inputs.ModifiersPending || !inputs.GpuReadsPages)
        return false;
    return TerrainHeightHasCpuPages(inputs);
}

bool TerrainHeightHasCpuPages(const TerrainHeightSourceInputs& inputs)
{
    return inputs.PagesOnLattice && inputs.CpuDirectoryFits && (inputs.GeneratedBase || inputs.CookedStoreOpen);
}

} // namespace GameEngine::TerrainECS
