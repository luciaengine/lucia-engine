#include "TileMap/CollisionEdgeMerge.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

constexpr uint32 kSquareTileId = 1;
constexpr uint32 kTriangleTileId = 2;
constexpr uint32 kNoCollisionTileId = 3;
constexpr uint32 kOneWayTileId = 4;
constexpr uint32 kIceTileId = 5;
constexpr uint32 kIceTriangleTileId = 6;
constexpr uint32 kSquarePlusExtraTileId = 7;
constexpr uint32 kNotchTileId = 8;

// Stable for the process so emitted rects can be compared against it.
const GUID& IceMaterial()
{
    static const GUID guid = GUID::Generate();
    return guid;
}

TileCollisionPolygon FullSquare()
{
    return TileCollisionPolygon{{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
}

// Right triangle filling the tile's lower-left half — asymmetric enough that
// every flip/transpose produces a distinct point set.
TileCollisionPolygon LowerLeftTriangle()
{
    return TileCollisionPolygon{{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}}};
}

// Concave: a cup with a notch cut out of its top. Its convex hull is the whole
// unit square, so a hull-based extrusion of it fills the notch in.
TileCollisionPolygon NotchedCup()
{
    return TileCollisionPolygon{{{0.0f, 0.0f},
                                 {1.0f, 0.0f},
                                 {1.0f, 1.0f},
                                 {0.8f, 1.0f},
                                 {0.8f, 0.25f},
                                 {0.2f, 0.25f},
                                 {0.2f, 1.0f},
                                 {0.0f, 1.0f}}};
}

// Asset is non-copyable, so tests build the set in place.
struct TileSetFixture
{
    TileSetAsset Asset{GUID::Generate(), "test.tileset"};

    TileSetFixture()
    {
        TileSetTile square;
        square.Id = kSquareTileId;
        square.CollisionPolygons.push_back(FullSquare());

        TileSetTile triangle;
        triangle.Id = kTriangleTileId;
        triangle.CollisionPolygons.push_back(LowerLeftTriangle());

        TileSetTile decor;
        decor.Id = kNoCollisionTileId;

        TileSetTile oneWay;
        oneWay.Id = kOneWayTileId;
        oneWay.CollisionPolygons.push_back(FullSquare());
        oneWay.CollisionMode = TileCollisionMode::OneWay;

        TileSetTile ice;
        ice.Id = kIceTileId;
        ice.CollisionPolygons.push_back(FullSquare());
        ice.PhysicsMaterial = IceMaterial();

        TileSetTile iceTriangle;
        iceTriangle.Id = kIceTriangleTileId;
        iceTriangle.CollisionPolygons.push_back(LowerLeftTriangle());
        iceTriangle.CollisionMode = TileCollisionMode::OneWay;
        iceTriangle.PhysicsMaterial = IceMaterial();

        // Authoring mistake: a full square AND a second polygon on one tile.
        TileSetTile squarePlusExtra;
        squarePlusExtra.Id = kSquarePlusExtraTileId;
        squarePlusExtra.CollisionPolygons.push_back(FullSquare());
        squarePlusExtra.CollisionPolygons.push_back(LowerLeftTriangle());

        TileSetTile notch;
        notch.Id = kNotchTileId;
        notch.CollisionPolygons.push_back(NotchedCup());

        Asset.Tiles() = {square,          triangle, decor, oneWay,
                         ice,             iceTriangle,
                         squarePlusExtra, notch};
    }
};

void SetChunkCell(TileMapChunk& chunk, int32 x, int32 y, TileCell cell)
{
    chunk.Cells[static_cast<size_t>(y * kChunkSize + x)] = cell;
}

} // namespace

TEST(CollisionEdgeMergeSquareDetect, RecognizesUnitSquareInAnyOrder)
{
    EXPECT_TRUE(IsFullSquarePolygon(FullSquare()));
    EXPECT_TRUE(IsFullSquarePolygon(
        TileCollisionPolygon{{{1.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}}}));
}

TEST(CollisionEdgeMergeSquareDetect, RejectsNonSquares)
{
    EXPECT_FALSE(IsFullSquarePolygon(LowerLeftTriangle()));
    // Half tile: four points but not the unit corners.
    EXPECT_FALSE(IsFullSquarePolygon(
        TileCollisionPolygon{{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}, {0.0f, 0.5f}}}));
    // Duplicate corner.
    EXPECT_FALSE(IsFullSquarePolygon(
        TileCollisionPolygon{{{0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}}));
}

TEST(CollisionEdgeMerge, EmptyChunkYieldsNothing)
{
    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(TileMapChunk{}, TileSetFixture().Asset);
    EXPECT_TRUE(shapes.IsEmpty());
}

TEST(CollisionEdgeMerge, TileWithoutCollisionYieldsNothing)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 3, 3, MakeCell(kNoCollisionTileId));
    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    EXPECT_TRUE(shapes.IsEmpty());
}

TEST(CollisionEdgeMerge, FullChunkMergesToOneRect)
{
    TileMapChunk chunk;
    for (int32 y = 0; y < kChunkSize; ++y)
        for (int32 x = 0; x < kChunkSize; ++x)
            SetChunkCell(chunk, x, y, MakeCell(kSquareTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_TRUE(shapes.Outlines.empty());
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{0.0f, 0.0f, 32.0f, 32.0f}));
}

TEST(CollisionEdgeMerge, RowMergesToOneRect)
{
    TileMapChunk chunk;
    for (int32 x = 0; x < kChunkSize; ++x)
        SetChunkCell(chunk, x, 5, MakeCell(kSquareTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{0.0f, 5.0f, 32.0f, 6.0f}));
}

TEST(CollisionEdgeMerge, ColumnMergesToOneRect)
{
    TileMapChunk chunk;
    for (int32 y = 0; y < kChunkSize; ++y)
        SetChunkCell(chunk, 7, y, MakeCell(kSquareTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{7.0f, 0.0f, 8.0f, 32.0f}));
}

TEST(CollisionEdgeMerge, CheckerboardStaysPerCell)
{
    TileMapChunk chunk;
    int32 count = 0;
    for (int32 y = 0; y < 8; ++y)
        for (int32 x = 0; x < 8; ++x)
            if (((x + y) & 1) == 0)
            {
                SetChunkCell(chunk, x, y, MakeCell(kSquareTileId));
                ++count;
            }

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    EXPECT_EQ(shapes.Rects.size(), static_cast<size_t>(count)); // 32 isolated cells
}

TEST(CollisionEdgeMerge, LShapeMergesToTwoRects)
{
    // 4-wide floor on row 0 plus a 1-wide column rising above its left end:
    // greedy scan takes the full floor first, then the remaining column.
    TileMapChunk chunk;
    for (int32 x = 0; x < 4; ++x)
        SetChunkCell(chunk, x, 0, MakeCell(kSquareTileId));
    for (int32 y = 1; y < 4; ++y)
        SetChunkCell(chunk, 0, y, MakeCell(kSquareTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 2u);
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{0.0f, 0.0f, 4.0f, 1.0f}));
    EXPECT_EQ(shapes.Rects[1], (CollisionRect{0.0f, 1.0f, 1.0f, 4.0f}));
}

TEST(CollisionEdgeMerge, FlippedSquareStillMerges)
{
    // Orientation bits never break the full-square case.
    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(kSquareTileId));
    SetChunkCell(chunk, 1, 0, MakeCell(kSquareTileId, true, false, false));
    SetChunkCell(chunk, 2, 0, MakeCell(kSquareTileId, false, true, true));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{0.0f, 0.0f, 3.0f, 1.0f}));
}

TEST(CollisionEdgeMerge, PolygonPassesThroughAtCellOrigin)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 2, 3, MakeCell(kTriangleTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    EXPECT_TRUE(shapes.Rects.empty());
    ASSERT_EQ(shapes.Outlines.size(), 1u);
    const auto& pts = shapes.Outlines[0].Points;
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_FLOAT_EQ(pts[0].x, 2.0f);
    EXPECT_FLOAT_EQ(pts[0].y, 3.0f);
    EXPECT_FLOAT_EQ(pts[1].x, 3.0f);
    EXPECT_FLOAT_EQ(pts[1].y, 3.0f);
    EXPECT_FLOAT_EQ(pts[2].x, 2.0f);
    EXPECT_FLOAT_EQ(pts[2].y, 4.0f);
}

TEST(CollisionEdgeMerge, FlipHMirrorsPolygonWithinCell)
{
    // Lower-left triangle flipped H becomes the lower-right triangle.
    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(kTriangleTileId, /*flipH*/ true));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Outlines.size(), 1u);
    const auto& pts = shapes.Outlines[0].Points;
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_FLOAT_EQ(pts[0].x, 1.0f);
    EXPECT_FLOAT_EQ(pts[0].y, 0.0f);
    EXPECT_FLOAT_EQ(pts[1].x, 0.0f);
    EXPECT_FLOAT_EQ(pts[1].y, 0.0f);
    EXPECT_FLOAT_EQ(pts[2].x, 1.0f);
    EXPECT_FLOAT_EQ(pts[2].y, 1.0f);
}

TEST(CollisionEdgeMerge, TransposeSwapsPolygonAxes)
{
    // Transpose reflects across the main diagonal; the lower-left triangle is
    // symmetric under it, so assert on a point-set that proves the swap ran:
    // flipV then transpose maps (0,0)->(1,0), (1,0)->(1,1), (0,1)->(0,0).
    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(kTriangleTileId, false, /*flipV*/ true, /*transpose*/ true));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Outlines.size(), 1u);
    const auto& pts = shapes.Outlines[0].Points;
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_FLOAT_EQ(pts[0].x, 1.0f);
    EXPECT_FLOAT_EQ(pts[0].y, 0.0f);
    EXPECT_FLOAT_EQ(pts[1].x, 1.0f);
    EXPECT_FLOAT_EQ(pts[1].y, 1.0f);
    EXPECT_FLOAT_EQ(pts[2].x, 0.0f);
    EXPECT_FLOAT_EQ(pts[2].y, 0.0f);
}

TEST(CollisionEdgeMerge, DegeneratePolygonIsSkipped)
{
    TileSetAsset asset(GUID::Generate(), "test.tileset");
    TileSetTile bad;
    bad.Id = 1;
    bad.CollisionPolygons.push_back(TileCollisionPolygon{{{0.0f, 0.0f}, {1.0f, 1.0f}}});
    asset.Tiles() = {bad};

    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(1));
    EXPECT_TRUE(BuildChunkCollisionShapes(chunk, asset).IsEmpty());
}

TEST(CollisionEdgeMerge, MixedSquareAndPolygonTilesSplitCorrectly)
{
    // A 3-long square floor with a triangle ramp at its right end.
    TileMapChunk chunk;
    for (int32 x = 0; x < 3; ++x)
        SetChunkCell(chunk, x, 0, MakeCell(kSquareTileId));
    SetChunkCell(chunk, 3, 0, MakeCell(kTriangleTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0], (CollisionRect{0.0f, 0.0f, 3.0f, 1.0f}));
    ASSERT_EQ(shapes.Outlines.size(), 1u);
}

TEST(CollisionEdgeMergeGrouping, OneWayDoesNotMergeIntoAdjacentSolid)
{
    // Four solid cells then four one-way cells on the same row. Merging across
    // the boundary would hand one side the other's mode.
    TileMapChunk chunk;
    for (int32 x = 0; x < 4; ++x)
        SetChunkCell(chunk, x, 0, MakeCell(kSquareTileId));
    for (int32 x = 4; x < 8; ++x)
        SetChunkCell(chunk, x, 0, MakeCell(kOneWayTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 2u);
    EXPECT_EQ(shapes.Rects[0],
              (CollisionRect{0.0f, 0.0f, 4.0f, 1.0f, TileCollisionMode::Solid, GUID()}));
    EXPECT_EQ(shapes.Rects[1],
              (CollisionRect{4.0f, 0.0f, 8.0f, 1.0f, TileCollisionMode::OneWay, GUID()}));
}

TEST(CollisionEdgeMergeGrouping, IceDoesNotMergeIntoAdjacentDefaultMaterial)
{
    TileMapChunk chunk;
    for (int32 x = 0; x < 3; ++x)
        SetChunkCell(chunk, x, 2, MakeCell(kSquareTileId));
    for (int32 x = 3; x < 6; ++x)
        SetChunkCell(chunk, x, 2, MakeCell(kIceTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 2u);
    EXPECT_EQ(shapes.Rects[0],
              (CollisionRect{0.0f, 2.0f, 3.0f, 3.0f, TileCollisionMode::Solid, GUID()}));
    EXPECT_EQ(shapes.Rects[1],
              (CollisionRect{3.0f, 2.0f, 6.0f, 3.0f, TileCollisionMode::Solid, IceMaterial()}));
}

TEST(CollisionEdgeMergeGrouping, LikeTilesStillMergeWithinTheirGroup)
{
    // Guards the opposite failure: grouping must not split a uniform run.
    TileMapChunk chunk;
    for (int32 x = 0; x < kChunkSize; ++x)
        SetChunkCell(chunk, x, 3, MakeCell(kOneWayTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0],
              (CollisionRect{0.0f, 3.0f, 32.0f, 4.0f, TileCollisionMode::OneWay, GUID()}));
}

TEST(CollisionEdgeMergeGrouping, OutlinesCarryModeAndMaterial)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 1, 1, MakeCell(kIceTriangleTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    EXPECT_TRUE(shapes.Rects.empty());
    ASSERT_EQ(shapes.Outlines.size(), 1u);
    EXPECT_EQ(shapes.Outlines[0].Mode, TileCollisionMode::OneWay);
    EXPECT_EQ(shapes.Outlines[0].Material, IceMaterial());
}

TEST(CollisionEdgeMergeGrouping, DefaultTilesCarryDefaultModeAndMaterial)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(kSquareTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    ASSERT_EQ(shapes.Rects.size(), 1u);
    EXPECT_EQ(shapes.Rects[0].Mode, TileCollisionMode::Solid);
    EXPECT_TRUE(shapes.Rects[0].Material.IsNull());
}

TEST(CollisionEdgeMergeTransform, InverseOfTextureLookupOrder)
{
    // The shader maps geometry->texture as transpose, flipH, flipV; collision
    // points live in texture space, so the point map must be the inverse:
    // flipV, flipH, transpose. For flipH+transpose that order sends (0.25, 0)
    // to (0, 0.75); the wrong (forward) order would give (0.75, 0) -> ... a
    // different point. Pin the composite here.
    const TileCell cell = MakeCell(1, /*flipH*/ true, /*flipV*/ false, /*transpose*/ true);
    const Mathematics::Vector2 mapped = TransformTileLocalPoint({0.25f, 0.0f}, cell);
    EXPECT_FLOAT_EQ(mapped.x, 0.0f);
    EXPECT_FLOAT_EQ(mapped.y, 0.75f);
}

// ---------------------------------------------------------------------------
// One cell, one shape; and every emitted outline is convex, because the baker
// extrudes each one through a convex hull.
// ---------------------------------------------------------------------------

namespace
{

// Signed area doubled; positive for counter-clockwise winding.
float32 SignedArea2(const std::vector<Mathematics::Vector2>& points)
{
    float32 sum = 0.0f;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Mathematics::Vector2& a = points[i];
        const Mathematics::Vector2& b = points[(i + 1) % points.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum;
}

bool IsConvex(const std::vector<Mathematics::Vector2>& points)
{
    if (points.size() < 3)
        return false;
    bool sawPositive = false;
    bool sawNegative = false;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Mathematics::Vector2& a = points[i];
        const Mathematics::Vector2& b = points[(i + 1) % points.size()];
        const Mathematics::Vector2& c = points[(i + 2) % points.size()];
        const float32 cross =
            (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (cross > 1.0e-5f)
            sawPositive = true;
        else if (cross < -1.0e-5f)
            sawNegative = true;
    }
    return !(sawPositive && sawNegative);
}

} // namespace

TEST(CollisionEdgeMerge, ATileCarryingASquareAndASecondPolygonYieldsOneShape)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 4, 4, MakeCell(kSquarePlusExtraTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);

    // The square already covers the cell; emitting the second polygon as well
    // would overlap two collision shapes on one cell.
    EXPECT_EQ(shapes.Rects.size(), 1u);
    EXPECT_TRUE(shapes.Outlines.empty());
}

TEST(CollisionEdgeMerge, AConcavePolygonIsDecomposedIntoConvexPieces)
{
    TileMapChunk chunk;
    SetChunkCell(chunk, 0, 0, MakeCell(kNotchTileId));

    const ChunkCollisionShapes shapes = BuildChunkCollisionShapes(chunk, TileSetFixture().Asset);
    EXPECT_TRUE(shapes.Rects.empty());
    ASSERT_GE(shapes.Outlines.size(), 2u) << "a concave outline cannot be one convex piece";

    float32 area = 0.0f;
    for (const CollisionOutline& outline : shapes.Outlines)
    {
        EXPECT_TRUE(IsConvex(outline.Points))
            << "the baker extrudes each outline through a convex hull, so a concave one "
               "silently gains the area its hull adds";
        area += std::fabs(SignedArea2(outline.Points)) * 0.5f;
    }

    // The cup: unit square minus the 0.6 x 0.75 notch.
    EXPECT_NEAR(area, 1.0f - 0.6f * 0.75f, 1.0e-4f);
}
