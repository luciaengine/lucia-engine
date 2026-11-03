#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "Mathematics/Vector2.h"
#include "TileMap/TerrainPeering.h"

#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace GameEngine
{
namespace TileMap
{

/** Normalized UV rectangle of a tile within its texture. */
struct TileUVRect
{
    float32 U0 = 0.0f;
    float32 V0 = 0.0f;
    float32 U1 = 1.0f;
    float32 V1 = 1.0f;

    bool operator==(const TileUVRect&) const = default;
};

/** One convex or concave collision outline in tile-local space ([0,1]² over the tile). */
struct TileCollisionPolygon
{
    std::vector<Mathematics::Vector2> Points;
};

/**
 * @brief How a tile's collision geometry blocks movement.
 *
 * OneWay geometry is solid only to a contact landing on its top face: a body
 * moving upward passes through, so a character can jump up through a platform
 * and come to rest on it falling back down. There is deliberately no
 * per-direction variant — the accept/reject test needs the contact normal and
 * relative velocity, which live at the physics contact, not in authoring data.
 */
enum class TileCollisionMode : uint8
{
    Solid,
    OneWay,
};

/** One frame of a tile's looping animation: another tile id in the same set. */
struct TileAnimationFrame
{
    uint32 TileId = 0;
    float32 DurationSeconds = 0.1f;
};

using TileCustomValue = std::variant<bool, int64, float64, std::string>;

/**
 * @brief One tile definition. Id is 1-based and stable (0 = empty cell);
 * atlas tiles carry a UV rect into the tileset's atlas texture, collection
 * tiles override with their own texture GUID.
 */
struct TileSetTile
{
    uint32 Id = 0;
    TileUVRect UV;
    GUID TextureOverride; // null => tileset atlas texture
    float32 Probability = 1.0f;
    std::vector<TileCollisionPolygon> CollisionPolygons;
    TileCollisionMode CollisionMode = TileCollisionMode::Solid;
    GUID PhysicsMaterial; // null => the collider's own friction/restitution
    std::vector<TileAnimationFrame> AnimationFrames;
    TerrainPeering Peering;
    std::map<std::string, TileCustomValue> CustomData;
};

struct TerrainDef
{
    std::string Name;
    uint32 ColorRGBA = 0xFFFFFFFFu;
};

struct TerrainSetDef
{
    std::string Name;
    TerrainMatchMode MatchMode = TerrainMatchMode::CornersAndSides;
    std::vector<TerrainDef> Terrains;
};

/**
 * @brief A named stamp saved with the tileset: a rectangle of cells, row-major
 * from the top-left like the palette lays tiles out. Cell ids index this
 * tileset, which is why the brush lives here and not with the map.
 */
struct TileBrushDef
{
    std::string Name;
    int32 Width = 0;
    int32 Height = 0;
    std::vector<uint32> Cells;
};

/** Ceiling on one brush's footprint; anything larger is rejected at load. */
inline constexpr int32 kMaxBrushAxisCells = 64;

/**
 * Hard ceilings on tileset geometry. A .tileset is a hand-editable text file,
 * so its grid parameters are foreign input: the tile table is sized by
 * columns * rows, which bounds the product as well as each axis. Anything
 * above these is rejected at load — never clamped, since a clamped grid would
 * silently re-number every tile a map already references.
 */
inline constexpr int32 kMaxAtlasAxisPx = 1 << 20;
inline constexpr int32 kMaxTileSetTiles = 65536;

/**
 * @brief External tileset asset (.tileset): an atlas texture sliced by a
 * uniform grid (margin/spacing), per-tile authored data (collision, animation,
 * probability, terrain peering, custom values), and terrain-set definitions.
 */
class TileSetAsset : public ::GameEngine::Asset
{
public:
    static constexpr int32 kSchemaVersion = 1;

    TileSetAsset(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::TileSet, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    GUID AtlasTexture() const { return m_AtlasTexture; }
    int32 AtlasWidthPx() const { return m_AtlasWidthPx; }
    int32 AtlasHeightPx() const { return m_AtlasHeightPx; }
    int32 TileWidthPx() const { return m_TileWidthPx; }
    int32 TileHeightPx() const { return m_TileHeightPx; }
    int32 MarginPx() const { return m_MarginPx; }
    int32 SpacingPx() const { return m_SpacingPx; }
    int32 Columns() const { return m_Columns; }
    int32 Rows() const { return m_Rows; }

    const std::vector<TileSetTile>& Tiles() const { return m_Tiles; }
    std::vector<TileSetTile>& Tiles() { return m_Tiles; }
    const TileSetTile* FindTile(uint32 tileId) const;
    TileSetTile* FindTile(uint32 tileId);

    const std::vector<TerrainSetDef>& TerrainSets() const { return m_TerrainSets; }
    std::vector<TerrainSetDef>& TerrainSets() { return m_TerrainSets; }
    const std::vector<TileBrushDef>& Brushes() const { return m_Brushes; }
    std::vector<TileBrushDef>& Brushes() { return m_Brushes; }

    void SetAtlasTexture(const GUID& texture, int32 atlasWidthPx, int32 atlasHeightPx);

    /**
     * Rebuilds the tile table from the grid parameters: tile ids are assigned
     * row-major (1-based) across the sliced grid. Authored per-tile data is
     * preserved by id where the id still exists after the re-slice.
     *
     * Returns false — leaving the asset EXACTLY as it was — when the atlas and
     * grid parameters slice to more than kMaxTileSetTiles tiles, so a hostile
     * atlas size cannot size an allocation. The refusal keeps the existing tile
     * table and its authoring: emptying it here would hand the next successful
     * slice a blank asset to write over the author's file.
     */
    bool SliceGrid(int32 tileWidthPx, int32 tileHeightPx, int32 marginPx, int32 spacingPx);

    /** How many columns/rows a grid slice yields for the given atlas + params (0 if invalid). */
    static int32 GridColumns(int32 atlasWidthPx, int32 tileWidthPx, int32 marginPx, int32 spacingPx);
    static int32 GridRows(int32 atlasHeightPx, int32 tileHeightPx, int32 marginPx, int32 spacingPx);

    /** Normalized UV rect of the grid cell (column, row); pure and testable. */
    static TileUVRect ComputeTileUV(int32 atlasWidthPx, int32 atlasHeightPx,
                                    int32 tileWidthPx, int32 tileHeightPx,
                                    int32 marginPx, int32 spacingPx,
                                    int32 column, int32 row);

private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    GUID m_AtlasTexture;
    int32 m_AtlasWidthPx = 0;
    int32 m_AtlasHeightPx = 0;
    int32 m_TileWidthPx = 0;
    int32 m_TileHeightPx = 0;
    int32 m_MarginPx = 0;
    int32 m_SpacingPx = 0;
    int32 m_Columns = 0;
    int32 m_Rows = 0;
    std::vector<TileSetTile> m_Tiles;
    std::vector<TerrainSetDef> m_TerrainSets;
    std::vector<TileBrushDef> m_Brushes;
};

} // namespace TileMap
} // namespace GameEngine
