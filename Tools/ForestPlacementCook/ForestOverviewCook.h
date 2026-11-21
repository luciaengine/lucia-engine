#pragma once

#include <array>
#include <filesystem>

struct ForestOverviewSource
{
    // Bounds of the top projection in model space. The first image row is at MinZ.
    double MinX;
    double MinZ;
    double MaxX;
    double MaxZ;
    double Height;
    unsigned TileSize;
};

void CookForestOverview(const std::filesystem::path& archive, const std::filesystem::path& image,
                        const std::filesystem::path& output, const ForestOverviewSource& source);
