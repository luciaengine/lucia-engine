#pragma once

#include "Thumbnails/ThumbnailImageOps.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace GameEngine
{

class AssetManager;
class GUID;

namespace Editor
{

/**
 * @brief Shared building blocks for the CPU-composited thumbnails: image
 * decode/encode, and the content stamp that names their cache files.
 *
 * The GPU thumbnail path exists for assets that need the real renderer (models,
 * materials). Tile grids and sprite frames do not — they are a crop and a blit,
 * so they composite on a worker thread and land in the cache as a PNG.
 *
 * Cache files are named after a stamp over the asset and everything it draws
 * with, so editing any source asks for a file that does not exist yet and
 * regenerates. That is the whole invalidation mechanism: there is no hook to
 * keep in sync with the writers.
 */

/** Decodes any stb-supported image to RGBA8. Empty on failure. */
ThumbnailImage DecodeImage(const std::filesystem::path& path);

/** Writes RGBA8 PNG, creating parent directories. False when the image is empty or the write fails. */
bool WritePng(const std::filesystem::path& path, const ThumbnailImage& image);

/** Last-write time, or a default-constructed time when the file cannot be stat'd. */
std::filesystem::file_time_type WriteTimeOrDefault(const std::filesystem::path& path);

void HashCombine(std::size_t& seed, std::size_t value);

/** Folds a dependency's identity and last-write time into a cache stamp. */
void HashPathAndWriteTime(std::size_t& seed, const std::filesystem::path& path);

/**
 * Drops this asset's *other* stamps. The identity prefix is what makes that
 * safe: the file name alone cannot say which asset a bare content hash came
 * from, and a sweep that guessed would delete other assets' thumbnails.
 */
void RemoveStaleCacheFiles(const std::filesystem::path& cacheRoot,
                           const std::string& identityPrefix,
                           const std::filesystem::path& keepPath);

/** Resolves an asset GUID to an absolute file path, or empty. */
std::filesystem::path ResolveAssetPathByGuid(AssetManager* assetManager, const GUID& guid,
                                             const std::filesystem::path& assetsRoot);

} // namespace Editor
} // namespace GameEngine
