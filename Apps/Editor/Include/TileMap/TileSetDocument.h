#pragma once

#include "AssetCore/GUID.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace GameEngine
{
namespace TileMap
{
class TileSetAsset;
}

namespace Editor
{

/**
 * @brief The `.tileset` file an authoring session is bound to: which file it is,
 * how it is opened without destroying what is already in it, and how it is
 * written back.
 *
 * Separate from the Tile Set Editor panel because the destructive answer is
 * always the same one — binding to a path must READ the file that is already
 * there. A panel that binds by starting a blank asset over an existing file
 * loses every authored polygon, mode and material in it, with no undo.
 */
class TileSetDocument
{
public:
    TileSetDocument();
    ~TileSetDocument();

    TileSetDocument(const TileSetDocument&) = delete;
    TileSetDocument& operator=(const TileSetDocument&) = delete;

    /**
     * Where a texture's tile set lives BY DEFAULT: the texture's own path,
     * `.tileset`. Only a guess — a tile set that already exists is found by its
     * atlas GUID instead (TileSetAtlasLookup.h), because a rename breaks the
     * stem while the GUID survives it.
     */
    static std::filesystem::path PathForAtlasTexture(const std::filesystem::path& texturePath);

    /** What OpenOrCreate did — the panel reports each differently. */
    enum class OpenOutcome
    {
        Opened,  // an existing file was read; its authoring is in the asset
        Created, // no file at that path yet; the asset starts empty
        Failed,  // a file is there but could not be read; nothing is bound
    };

    /**
     * Binds to `path`. An existing file is READ, so every authored polygon,
     * collision mode, material and terrain annotation in it survives; only a
     * path with no file behind it starts an empty asset. Writes nothing.
     *
     * A file that fails to parse leaves the document bound to whatever it held
     * before, because overwriting a file the editor could not read is the one
     * outcome from which the author cannot recover.
     */
    OpenOutcome OpenOrCreate(const GUID& guid, const std::filesystem::path& path);

    /** What Save did, or refused to do. */
    enum class SaveOutcome
    {
        Written,       // the file on disk is this document
        ChangedOnDisk, // someone else wrote it since this document read it; nothing was written
        Failed,        // serialization or the write itself failed; the old file is intact
    };

    /**
     * Serializes the asset to its bound path.
     *
     * Refuses when the bytes on disk are no longer the bytes this document read
     * (a second panel on the same file, an external edit, a re-import): this
     * document's copy has none of those changes in it, so writing would drop
     * them silently. Re-open to take the other side, or AcceptDiskState to keep
     * this one.
     */
    SaveOutcome Save();

    /**
     * Whether the file on disk is no longer the one this document read — the
     * same condition Save refuses on. Callers that mutate the asset before
     * saving ask first, so a refused save does not leave them holding changes
     * they were told were not written.
     */
    bool HasChangedOnDisk() const;

    /**
     * Declares the file as it stands now to be the version this document is
     * based on, so the next Save overwrites it. The author's answer to
     * ChangedOnDisk when they want their copy to win.
     */
    void AcceptDiskState();

    /** The open asset, or null when nothing is bound. */
    TileMap::TileSetAsset* Asset() { return m_Asset.get(); }
    const TileMap::TileSetAsset* Asset() const { return m_Asset.get(); }
    bool IsOpen() const { return m_Asset != nullptr; }

    const std::filesystem::path& Path() const { return m_Path; }

    /** Whether any tile carries data a re-slice could not reproduce. */
    bool HasAuthoredTileData() const;

    /**
     * Whether re-slicing to this grid would strip authored data off tiles.
     *
     * Tile ids are grid positions (row-major, 1-based), so a grid whose column
     * or row count differs re-points every id at a different image region: the
     * authoring survives the re-slice by id but no longer describes the tile it
     * is on. Tiles falling off the end of a smaller grid lose it outright.
     */
    bool ReSliceDropsAuthoring(int32_t tileWidthPx, int32_t tileHeightPx, int32_t marginPx,
                               int32_t spacingPx) const;

private:
    std::unique_ptr<TileMap::TileSetAsset> m_Asset;
    std::filesystem::path m_Path;
    // FNV-1a over the file's bytes as this document last saw them; 0 means "no
    // file was there". Save compares it against disk to catch a write it would
    // otherwise silently discard.
    uint64_t m_DiskContentHash = 0;
};

} // namespace Editor
} // namespace GameEngine
