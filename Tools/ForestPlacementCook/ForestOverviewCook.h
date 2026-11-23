#pragma once

#include <filesystem>

namespace GameEngine
{

struct ForestOverviewSource
{
    // Bounds of the top projection in model space. The first image row is at MinZ.
    double MinX;
    double MinZ;
    double MaxX;
    double MaxZ;
    double Height; // Canopy height above the root at unit scale, in meters.
    unsigned TileSize; // Atlas pixels per page tile.
};

/// Writes a GLB with one canopy quad per archive page and an embedded atlas. Throws std::runtime_error.
void CookForestOverview(const std::filesystem::path& archive, const std::filesystem::path& image,
                        const std::filesystem::path& output, const ForestOverviewSource& source);

} // namespace GameEngine
