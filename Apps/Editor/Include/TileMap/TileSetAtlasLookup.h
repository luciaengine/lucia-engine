#pragma once

#include "AssetCore/GUID.h"

#include <filesystem>

namespace GameEngine
{
class AssetRegistry;

namespace Editor
{

/**
 * The `.tileset` already bound to `atlasTexture`, or an empty path when no
 * registered tile set names that texture as its atlas.
 *
 * Deriving the path from the texture's stem is only a guess, and a rename
 * breaks it: every LDtk/Tiled import and every user rename produces
 * `forest.tileset` beside `atlas.png`, after which the stem derives a path with
 * no file behind it — so the panel creates a second, blank tile set and the
 * author's real one goes invisible to the whole workflow. The atlas GUID is the
 * durable link, so the lookup on it is what must be tried first; the stem
 * remains the fallback for a tile set that does not exist yet.
 *
 * Reads every registered `.tileset` from disk, so this answers a user action —
 * never a per-frame path.
 *
 * Ties are broken deterministically, because several tile sets may legitimately
 * slice one atlas and the answer must not depend on registry iteration order:
 * `preferredPath` wins when it is among the matches, otherwise the
 * lexicographically first path does.
 */
std::filesystem::path FindTileSetForAtlas(AssetRegistry& registry, const GUID& atlasTexture,
                                          const std::filesystem::path& preferredPath = {});

} // namespace Editor
} // namespace GameEngine
