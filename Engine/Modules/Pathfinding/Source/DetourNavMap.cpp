#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/PathBuffer.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

#if GE_PATHFINDING_BACKEND_DETOUR
#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#endif

namespace GameEngine::Pathfinding
{

#if GE_PATHFINDING_BACKEND_DETOUR

static_assert(sizeof(int) == sizeof(uint32), "Recast API expects int-sized indices");

static constexpr int32 kMaxPathPolys = 256;
static constexpr int32 kMaxStraightPathPoints = 256;
static constexpr float32 kPolySearchExtent[3] = {2.0f, 4.0f, 2.0f};

struct DetourNavMap::Impl
{
    dtNavMesh* NavMesh = nullptr;
    dtNavMeshQuery* NavQuery = nullptr;
    dtQueryFilter Filter;

    std::vector<float32> DebugVertices;
    std::vector<uint32> DebugIndices;
    std::vector<uint64> TileDigests;
    NavMeshSettings BakeSettings;
    float32 BakeMinY = 0;
    float32 BakeMaxY = 0;
    int32 BakeTilesX = 0;

    ~Impl()
    {
        if (NavQuery)
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
        }
        if (NavMesh)
        {
            dtFreeNavMesh(NavMesh);
            NavMesh = nullptr;
        }
    }

    void BuildDebugMesh()
    {
        DebugVertices.clear();
        DebugIndices.clear();

        if (!NavMesh)
            return;

        // Extract debug geometry from the navmesh (use const pointer to
        // select the public const overload of getTile).
        const dtNavMesh* navMeshConst = NavMesh;
        for (int32 i = 0; i < navMeshConst->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMeshConst->getTile(i);
            if (!tile || !tile->header)
                continue;

            const uint32 baseVertex = static_cast<uint32>(DebugVertices.size() / 3);

            for (int32 v = 0; v < tile->header->vertCount; ++v)
            {
                const float32* vert = &tile->verts[v * 3];
                DebugVertices.push_back(vert[0]);
                DebugVertices.push_back(vert[1]);
                DebugVertices.push_back(vert[2]);
            }

            for (int32 p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    continue;

                // Triangulate the polygon as a fan
                for (int32 t = 2; t < static_cast<int32>(poly.vertCount); ++t)
                {
                    DebugIndices.push_back(baseVertex + poly.verts[0]);
                    DebugIndices.push_back(baseVertex + poly.verts[t - 1]);
                    DebugIndices.push_back(baseVertex + poly.verts[t]);
                }
            }
        }
    }
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

namespace
{

struct TileData
{
    std::unique_ptr<uint8, decltype(&dtFree)> Bytes{nullptr, dtFree};
    int32 Size = 0;
};

constexpr int32 kPreferredTileCells = 128;
constexpr int32 kMaxBakeTiles = 1024;

struct TileInput
{
    std::vector<int32> Indices;
    uint64 Digest = Hashing::kFnv1a64OffsetBasis;
};

struct TileChange
{
    TileData Replacement;
    TileData Previous;
    dtTileRef PreviousReference = 0;
    dtTileRef ReplacementReference = 0;
};

auto BakeSettingsKey(const NavMeshSettings& settings)
{
    return std::tie(settings.CellSize, settings.CellHeight, settings.AgentRadius, settings.AgentHeight,
                    settings.AgentMaxClimb, settings.AgentMaxSlope, settings.RegionMinSize,
                    settings.RegionMergeSize, settings.EdgeMaxLen, settings.EdgeMaxError,
                    settings.DetailSampleDist, settings.DetailSampleMaxError, settings.VertsPerPoly);
}

int32 ComputeTileIndex(float32 coordinate, float32 origin, float32 tileSize, int32 count)
{
    return std::clamp(static_cast<int32>(std::floor((coordinate - origin) / tileSize)), 0, count - 1);
}

void WriteNavBytes(std::span<uint8> bytes, size_t& offset, const void* source, size_t count)
{
    std::memcpy(bytes.data() + offset, source, count);
    offset += count;
}

bool ReadNavBytes(std::span<const uint8> bytes, size_t& offset, void* destination, size_t count)
{
    if (count > bytes.size() - offset)
        return false;
    std::memcpy(destination, bytes.data() + offset, count);
    offset += count;
    return true;
}

rcConfig MakeBakeConfig(const NavMeshSettings& settings)
{
    rcConfig config = {};
    config.cs = settings.CellSize;
    config.ch = settings.CellHeight;
    config.walkableSlopeAngle = settings.AgentMaxSlope;
    config.walkableHeight = static_cast<int32>(std::ceil(settings.AgentHeight / settings.CellHeight));
    config.walkableClimb = static_cast<int32>(std::floor(settings.AgentMaxClimb / settings.CellHeight));
    config.walkableRadius = static_cast<int32>(std::ceil(settings.AgentRadius / settings.CellSize));
    config.maxEdgeLen = static_cast<int32>(settings.EdgeMaxLen / settings.CellSize);
    config.maxSimplificationError = settings.EdgeMaxError;
    config.minRegionArea = static_cast<int32>(settings.RegionMinSize * settings.RegionMinSize);
    config.mergeRegionArea = static_cast<int32>(settings.RegionMergeSize * settings.RegionMergeSize);
    config.maxVertsPerPoly = settings.VertsPerPoly;
    config.detailSampleDist = settings.DetailSampleDist < 0.9f ? 0.0f : settings.CellSize * settings.DetailSampleDist;
    config.detailSampleMaxError = settings.CellHeight * settings.DetailSampleMaxError;
    return config;
}

bool BakeTile(const NavMeshSettings& settings, const rcConfig& config,
              const DetourNavMap::InputGeometry& geometry, std::span<const int32> indices,
              int32 tileX, int32 tileZ, TileData& output)
{
    if (indices.empty())
        return true;

    rcContext context;
    const std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> heightfield(
        rcAllocHeightfield(), rcFreeHeightField);
    if (!heightfield || !rcCreateHeightfield(&context, *heightfield, config.width, config.height,
                                              config.bmin, config.bmax, config.cs, config.ch))
        return false;

    const int32 vertexCount = static_cast<int32>(geometry.VertexCount);
    const int32 triangleCount = static_cast<int32>(indices.size() / 3);
    std::vector<uint8> areas(triangleCount);
    rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, geometry.Vertices, vertexCount,
                            indices.data(), triangleCount, areas.data());
    if (!rcRasterizeTriangles(&context, geometry.Vertices, vertexCount, indices.data(), areas.data(),
                              triangleCount, *heightfield, config.walkableClimb))
        return false;

    rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *heightfield);
    rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *heightfield);
    rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *heightfield);

    const std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)> compact(
        rcAllocCompactHeightfield(), rcFreeCompactHeightfield);
    if (!compact || !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb,
                                                *heightfield, *compact) ||
        !rcErodeWalkableArea(&context, config.walkableRadius, *compact) ||
        !rcBuildDistanceField(&context, *compact) ||
        !rcBuildRegions(&context, *compact, config.borderSize, config.minRegionArea, config.mergeRegionArea))
        return false;

    const std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)> contours(rcAllocContourSet(), rcFreeContourSet);
    if (!contours || !rcBuildContours(&context, *compact, config.maxSimplificationError, config.maxEdgeLen, *contours))
        return false;
    if (contours->nconts == 0)
        return true;

    const std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)> mesh(rcAllocPolyMesh(), rcFreePolyMesh);
    if (!mesh || !rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *mesh))
        return false;
    if (mesh->npolys == 0)
        return true;

    const std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)> detail(
        rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);
    if (!detail || !rcBuildPolyMeshDetail(&context, *mesh, *compact, config.detailSampleDist,
                                         config.detailSampleMaxError, *detail))
        return false;
    std::fill_n(mesh->flags, mesh->npolys, 1);

    dtNavMeshCreateParams params = {};
    params.verts = mesh->verts;
    params.vertCount = mesh->nverts;
    params.polys = mesh->polys;
    params.polyAreas = mesh->areas;
    params.polyFlags = mesh->flags;
    params.polyCount = mesh->npolys;
    params.nvp = mesh->nvp;
    params.detailMeshes = detail->meshes;
    params.detailVerts = detail->verts;
    params.detailVertsCount = detail->nverts;
    params.detailTris = detail->tris;
    params.detailTriCount = detail->ntris;
    params.walkableHeight = settings.AgentHeight;
    params.walkableRadius = settings.AgentRadius;
    params.walkableClimb = settings.AgentMaxClimb;
    rcVcopy(params.bmin, mesh->bmin);
    rcVcopy(params.bmax, mesh->bmax);
    params.cs = config.cs;
    params.ch = config.ch;
    params.buildBvTree = true;

    params.tileX = tileX;
    params.tileY = tileZ;

    uint8* data = nullptr;
    if (!dtCreateNavMeshData(&params, &data, &output.Size))
        return false;
    output.Bytes.reset(data);
    return true;
}

std::vector<TileInput> BinTriangles(const DetourNavMap::InputGeometry& geometry, const rcConfig& config,
                                    int32 tilesX, int32 tilesZ)
{
    const float32 tileSize = config.tileSize * config.cs;
    const float32 border = config.borderSize * config.cs;
    std::vector<TileInput> tiles(static_cast<size_t>(tilesX) * tilesZ);
    for (uint32 triangle = 0; triangle < geometry.TriangleCount; ++triangle)
    {
        const uint32* indices = geometry.Indices + static_cast<size_t>(triangle) * 3;
        const float32* vertexA = geometry.Vertices + static_cast<size_t>(indices[0]) * 3;
        const float32* vertexB = geometry.Vertices + static_cast<size_t>(indices[1]) * 3;
        const float32* vertexC = geometry.Vertices + static_cast<size_t>(indices[2]) * 3;
        const int32 minX = ComputeTileIndex(std::min({vertexA[0], vertexB[0], vertexC[0]}) - border, config.bmin[0], tileSize, tilesX);
        const int32 maxX = ComputeTileIndex(std::max({vertexA[0], vertexB[0], vertexC[0]}) + border, config.bmin[0], tileSize, tilesX);
        const int32 minZ = ComputeTileIndex(std::min({vertexA[2], vertexB[2], vertexC[2]}) - border, config.bmin[2], tileSize, tilesZ);
        const int32 maxZ = ComputeTileIndex(std::max({vertexA[2], vertexB[2], vertexC[2]}) + border, config.bmin[2], tileSize, tilesZ);
        uint64 digest = Hashing::Fnv1a64(vertexA, 3 * sizeof(float32));
        digest = Hashing::Fnv1a64(vertexB, 3 * sizeof(float32), digest);
        digest = Hashing::Fnv1a64(vertexC, 3 * sizeof(float32), digest);
        for (int32 z = minZ; z <= maxZ; ++z)
        {
            for (int32 x = minX; x <= maxX; ++x)
            {
                TileInput& tile = tiles[static_cast<size_t>(z) * tilesX + x];
                tile.Indices.insert(tile.Indices.end(), indices, indices + 3);
                tile.Digest = Hashing::Fnv1a64Value(tile.Digest, digest);
            }
        }
    }
    return tiles;
}

rcConfig MakeTileConfig(rcConfig config, int32 x, int32 z)
{
    const float32 tileSize = config.tileSize * config.cs;
    const float32 border = config.borderSize * config.cs;
    config.width = config.height = config.tileSize + 2 * config.borderSize;
    config.bmax[0] = config.bmin[0] + (x + 1) * tileSize + border;
    config.bmax[2] = config.bmin[2] + (z + 1) * tileSize + border;
    config.bmin[0] += x * tileSize - border;
    config.bmin[2] += z * tileSize - border;
    return config;
}

bool ApplyTileChanges(dtNavMesh& mesh, std::span<TileChange> changes)
{
    size_t committed = 0;
    for (TileChange& change : changes)
    {
        if (change.PreviousReference && dtStatusFailed(mesh.removeTile(change.PreviousReference, nullptr, nullptr)))
            break;
        ++committed;
        if (change.Replacement.Bytes)
        {
            if (dtStatusFailed(mesh.addTile(change.Replacement.Bytes.get(), change.Replacement.Size,
                                            DT_TILE_FREE_DATA, 0, &change.ReplacementReference)))
                break;
            change.Replacement.Bytes.release();
        }
    }
    if (committed == changes.size() &&
        (changes.empty() || !changes.back().Replacement.Bytes))
        return true;

    for (TileChange& change : changes.first(committed))
    {
        if (change.ReplacementReference)
            mesh.removeTile(change.ReplacementReference, nullptr, nullptr);
    }
    for (TileChange& change : changes.first(committed))
    {
        if (change.Previous.Bytes &&
            dtStatusSucceed(mesh.addTile(change.Previous.Bytes.get(), change.Previous.Size,
                                        DT_TILE_FREE_DATA, change.PreviousReference, nullptr)))
            change.Previous.Bytes.release();
    }
    return false;
}

bool InitializeTileGrid(dtNavMesh& mesh, const rcConfig& config, float32 tileWorldSize, size_t tileCount)
{
    dtNavMeshParams params{};
    rcVcopy(params.orig, config.bmin);
    params.tileWidth = params.tileHeight = tileWorldSize;
    params.maxTiles = static_cast<int32>(tileCount);
    // Keep enough salt bits to invalidate paths through replaced tiles.
    constexpr unsigned int kTileAndPolygonBits = 22;
    params.maxPolys = 1 << (kTileAndPolygonBits - std::bit_width(static_cast<unsigned int>(tileCount - 1)));
    return dtStatusSucceed(mesh.init(&params));
}

} // namespace

bool DetourNavMap::Build(const NavMeshSettings& settings, const InputGeometry& geometry)
{
    if (!geometry.Vertices || geometry.VertexCount == 0 ||
        !geometry.Indices || geometry.TriangleCount == 0 ||
        geometry.VertexCount > std::numeric_limits<int32>::max() ||
        geometry.TriangleCount > std::numeric_limits<int32>::max() ||
        !(settings.CellSize > 0 && settings.CellHeight > 0))
        return false;

    const std::span indices(geometry.Indices, static_cast<size_t>(geometry.TriangleCount) * 3);
    if (std::ranges::any_of(indices, [&geometry](uint32 index) { return index >= geometry.VertexCount; }))
        return false;

    rcConfig config = MakeBakeConfig(settings);
    rcCalcBounds(geometry.Vertices, static_cast<int32>(geometry.VertexCount), config.bmin, config.bmax);
    constexpr float32 kMaxGridCells = static_cast<float32>(std::numeric_limits<int32>::max() / 4);
    if (!((config.bmax[0] - config.bmin[0]) / config.cs <= kMaxGridCells &&
          (config.bmax[2] - config.bmin[2]) / config.cs <= kMaxGridCells))
        return false;
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);
    if (config.width <= 0 || config.height <= 0)
        return false;

    int32 tilesX = 0;
    int32 tilesZ = 0;
    size_t tileCount = 0;
    config.tileSize = kPreferredTileCells;
    for (;; config.tileSize *= 2)
    {
        tilesX = (config.width - 1) / config.tileSize + 1;
        tilesZ = (config.height - 1) / config.tileSize + 1;
        tileCount = static_cast<size_t>(tilesX) * tilesZ;
        if (tileCount <= kMaxBakeTiles)
            break;
    }

    const bool isTiled = tileCount > 1;
    const float32 tileWorldSize = config.tileSize * config.cs;
    const dtNavMeshParams* previousParameters = IsBuilt() ? m_Impl->NavMesh->getParams() : nullptr;
    const bool canReuse = isTiled && previousParameters && m_Impl->TileDigests.size() == tileCount &&
        m_Impl->BakeTilesX == tilesX && BakeSettingsKey(m_Impl->BakeSettings) == BakeSettingsKey(settings) &&
        m_Impl->BakeMinY <= config.bmin[1] && m_Impl->BakeMaxY >= config.bmax[1] &&
        previousParameters->orig[0] == config.bmin[0] && previousParameters->orig[2] == config.bmin[2] &&
        previousParameters->tileWidth == tileWorldSize;
    std::unique_ptr<Impl> candidate;
    if (!canReuse)
    {
        candidate = std::make_unique<Impl>();
        candidate->NavMesh = dtAllocNavMesh();
        if (!candidate->NavMesh)
            return false;
    }

    if (!isTiled)
    {
        config.tileSize = 0;
        const std::span signedIndices(reinterpret_cast<const int32*>(indices.data()), indices.size());
        TileData tile;
        if (!BakeTile(settings, config, geometry, signedIndices, 0, 0, tile))
            return false;
        if (tile.Bytes)
        {
            if (dtStatusFailed(candidate->NavMesh->init(tile.Bytes.get(), tile.Size, DT_TILE_FREE_DATA)))
                return false;
            tile.Bytes.release();
        }
        else if (!InitializeTileGrid(*candidate->NavMesh, config, tileWorldSize, 1))
            return false;
    }
    else
    {
        Impl& target = canReuse ? *m_Impl : *candidate;
        dtNavMesh& mesh = *target.NavMesh;
        if (canReuse)
        {
            config.bmin[1] = target.BakeMinY;
            config.bmax[1] = target.BakeMaxY;
        }
        else
        {
            // Keep the voxel origin stable when a local edit adds short geometry.
            config.bmin[1] -= config.walkableHeight * config.ch;
            config.bmax[1] += config.walkableHeight * config.ch;
            if (!InitializeTileGrid(mesh, config, tileWorldSize, tileCount))
                return false;
        }

        constexpr int32 kRecastBorderCells = 3;
        config.borderSize = config.walkableRadius + kRecastBorderCells;
        const auto inputs = BinTriangles(geometry, config, tilesX, tilesZ);
        std::vector<TileChange> changes;
        for (size_t index = 0; index < tileCount; ++index)
        {
            const int32 x = static_cast<int32>(index % tilesX);
            const int32 z = static_cast<int32>(index / tilesX);
            const dtMeshTile* old = mesh.getTileAt(x, z, 0);
            const TileInput& input = inputs[index];
            if (canReuse && input.Digest == target.TileDigests[index])
            {
                continue;
            }

            TileChange& change = changes.emplace_back();
            if (!BakeTile(settings, MakeTileConfig(config, x, z), geometry, input.Indices, x, z, change.Replacement))
                return false;
            if (old)
            {
                change.PreviousReference = mesh.getTileRef(old);
                change.Previous.Size = old->dataSize;
                change.Previous.Bytes.reset(static_cast<uint8*>(dtAlloc(old->dataSize, DT_ALLOC_PERM)));
                if (!change.Previous.Bytes)
                    return false;
                std::memcpy(change.Previous.Bytes.get(), old->data, old->dataSize);
            }
        }
        if (!ApplyTileChanges(mesh, changes))
            return false;

        target.TileDigests.resize(tileCount);
        std::ranges::transform(inputs, target.TileDigests.begin(), &TileInput::Digest);
        target.BakeSettings = settings;
        target.BakeMinY = config.bmin[1];
        target.BakeMaxY = config.bmax[1];
        target.BakeTilesX = tilesX;
        if (canReuse)
        {
            if (!changes.empty())
                target.BuildDebugMesh();
            return true;
        }
    }

    candidate->NavQuery = dtAllocNavMeshQuery();
    if (!candidate->NavQuery || dtStatusFailed(candidate->NavQuery->init(candidate->NavMesh, 2048)))
        return false;
    candidate->BuildDebugMesh();
    m_Impl = std::move(candidate);
    return true;
}

bool DetourNavMap::IsBuilt() const
{
    return m_Impl && m_Impl->NavMesh != nullptr && m_Impl->NavQuery != nullptr;
}

PathStatus DetourNavMap::FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const
{
    if (!IsBuilt())
        return PathStatus::Failed;

    const float32 startPos[3] = {request.StartX, request.StartY, request.StartZ};
    const float32 endPos[3] = {request.EndX, request.EndY, request.EndZ};
    const float32 extent[3] = {request.AgentRadius * 4.0f, request.AgentHeight, request.AgentRadius * 4.0f};

    dtPolyRef startRef = 0, endRef = 0;
    float32 nearestStartPos[3], nearestEndPos[3];

    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, extent, &m_Impl->Filter,
                                                         &startRef, nearestStartPos);
    if (dtStatusFailed(status) || startRef == 0)
        return PathStatus::Failed;

    status = m_Impl->NavQuery->findNearestPoly(endPos, extent, &m_Impl->Filter,
                                                &endRef, nearestEndPos);
    if (dtStatusFailed(status) || endRef == 0)
        return PathStatus::Failed;

    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;
    const dtStatus findPathStatus = m_Impl->NavQuery->findPath(
        startRef, endRef, nearestStartPos, nearestEndPos,
        &m_Impl->Filter, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(findPathStatus) || pathPolyCount == 0)
        return PathStatus::Failed;

    float32 straightPath[kMaxStraightPathPoints * 3];
    uint8 straightPathFlags[kMaxStraightPathPoints];
    dtPolyRef straightPathPolys[kMaxStraightPathPoints];
    int32 straightPathCount = 0;

    status = m_Impl->NavQuery->findStraightPath(nearestStartPos, nearestEndPos,
                                                  pathPolys, pathPolyCount,
                                                  straightPath, straightPathFlags,
                                                  straightPathPolys, &straightPathCount,
                                                  kMaxStraightPathPoints);
    if (dtStatusFailed(status) || straightPathCount == 0)
        return PathStatus::Failed;

    std::vector<PathPoint> points(straightPathCount);
    for (int32 i = 0; i < straightPathCount; ++i)
    {
        points[i].X = straightPath[i * 3 + 0];
        points[i].Y = straightPath[i * 3 + 1];
        points[i].Z = straightPath[i * 3 + 2];
    }

    outPath = pathBuffer.AllocatePath(points.data(), static_cast<uint32>(points.size()));

    // Check the findPath status (not findStraightPath) for partial results.
    bool isPartial = dtStatusDetail(findPathStatus, DT_PARTIAL_RESULT);
    return isPartial ? PathStatus::Partial : PathStatus::Complete;
}

bool DetourNavMap::IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const
{
    if (!IsBuilt())
        return false;

    const float32 pos[3] = {x, y, z};
    const float32 extent[3] = {radius, radius * 2.0f, radius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Check XZ distance to nearest navigable point (height is checked separately
    // with a more generous tolerance since navmesh Y can differ from query Y due
    // to terrain slope within a polygon).
    const float32 dx = nearestPos[0] - x;
    const float32 dz = nearestPos[2] - z;
    const float32 distXZSq = dx * dx + dz * dz;
    const float32 dy = std::abs(nearestPos[1] - y);
    return distXZSq <= radius * radius && dy <= radius * 2.0f;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                             float32& outX, float32& outY, float32& outZ) const
{
    if (!IsBuilt())
        return false;

    const float32 pos[3] = {x, y, z};
    const float32 extent[3] = {searchRadius, searchRadius, searchRadius};

    dtPolyRef ref = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(pos, extent, &m_Impl->Filter, &ref, nearestPos);
    if (dtStatusFailed(status) || ref == 0)
        return false;

    // Reject results outside the requested search radius in the XZ plane
    // (Detour's extent is an AABB, so corner results can exceed the radius).
    const float32 dx = nearestPos[0] - x;
    const float32 dz = nearestPos[2] - z;
    if (dx * dx + dz * dz > searchRadius * searchRadius)
        return false;

    outX = nearestPos[0];
    outY = nearestPos[1];
    outZ = nearestPos[2];
    return true;
}

// NOTE: The start position is snapped to the nearest point on the navmesh via
// findNearestPoly before the raycast is performed. This means the actual ray
// origin may differ from (startX, startY, startZ) if the start is off-mesh.
// The returned hit point is along the snapped ray, not the original ray.
// Callers that need the exact ray origin should verify the start is on the
// navmesh (e.g., via IsPointNavigable) before calling Raycast.
bool DetourNavMap::Raycast(float32 startX, float32 startY, float32 startZ,
                            float32 endX, float32 endY, float32 endZ,
                            float32& hitX, float32& hitY, float32& hitZ) const
{
    if (!IsBuilt())
        return false;

    const float32 startPos[3] = {startX, startY, startZ};
    const float32 endPos[3] = {endX, endY, endZ};

    dtPolyRef startRef = 0;
    float32 nearestPos[3];
    dtStatus status = m_Impl->NavQuery->findNearestPoly(startPos, kPolySearchExtent,
                                                         &m_Impl->Filter, &startRef, nearestPos);
    if (dtStatusFailed(status) || startRef == 0)
        return false;

    float32 t = 0.0f;
    float32 hitNormal[3];
    dtPolyRef pathPolys[kMaxPathPolys];
    int32 pathPolyCount = 0;

    status = m_Impl->NavQuery->raycast(startRef, nearestPos, endPos, &m_Impl->Filter,
                                        &t, hitNormal, pathPolys, &pathPolyCount, kMaxPathPolys);
    if (dtStatusFailed(status))
        return false;

    if (t >= 1.0f)
        return false; // No hit, ray reached end

    hitX = nearestPos[0] + (endPos[0] - nearestPos[0]) * t;
    hitY = nearestPos[1] + (endPos[1] - nearestPos[1]) * t;
    hitZ = nearestPos[2] + (endPos[2] - nearestPos[2]) * t;
    return true;
}

// Serialization format (supports multi-tile navmeshes):
// [magic "DTNM"][version uint32][dtNavMeshParams (28 bytes)][tileCount uint32]
// For each tile: [tileRef uint64][dataSize uint32][tile data bytes]
// Optional suffix: [tiled bake tag][bake layout][one digest per grid tile]
static constexpr char kSerializeMagic[4] = {'D', 'T', 'N', 'M'};
static constexpr uint32 kSerializeVersion = 1;
static constexpr char kTiledBakeTag[4] = {'L', 'T', 'B', '1'};

struct TiledBakeMemo
{
    int32 Columns = 0;
    float32 MinY = 0;
    float32 MaxY = 0;
    NavMeshSettings Settings;
};
static_assert(sizeof(TiledBakeMemo) == 16 * sizeof(uint32));

bool DetourNavMap::Serialize(std::vector<uint8>& outData) const
{
    if (!IsBuilt())
        return false;

    const dtNavMesh* navMesh = m_Impl->NavMesh;
    const dtNavMeshParams* meshParams = navMesh->getParams();

    // Count active tiles and compute total size
    uint32 tileCount = 0;
    size_t totalTileDataSize = 0;
    for (int32 tileIndex = 0; tileIndex < navMesh->getMaxTiles(); ++tileIndex)
    {
        const dtMeshTile* tile = navMesh->getTile(tileIndex);
        if (!tile || !tile->header || tile->dataSize == 0)
            continue;
        ++tileCount;
        totalTileDataSize += tile->dataSize;
    }

    // Header: magic(4) + version(4) + params(28) + tileCount(4) = 40
    // Per tile: tileRef(8) + dataSize(4) + data(variable)
    const size_t headerSize = 4 + sizeof(uint32) + sizeof(dtNavMeshParams) + sizeof(uint32);
    const size_t perTileOverhead = sizeof(uint64) + sizeof(uint32);
    const size_t digestBytes = m_Impl->TileDigests.size() * sizeof(uint64);
    const size_t memoBytes = digestBytes ? sizeof(kTiledBakeTag) + sizeof(TiledBakeMemo) + digestBytes : 0;
    outData.resize(headerSize + tileCount * perTileOverhead + totalTileDataSize + memoBytes);

    size_t offset = 0;

    WriteNavBytes(outData, offset, kSerializeMagic, 4);
    WriteNavBytes(outData, offset, &kSerializeVersion, sizeof(uint32));
    WriteNavBytes(outData, offset, meshParams, sizeof(dtNavMeshParams));
    WriteNavBytes(outData, offset, &tileCount, sizeof(uint32));

    for (int32 tileIndex = 0; tileIndex < navMesh->getMaxTiles(); ++tileIndex)
    {
        const dtMeshTile* tile = navMesh->getTile(tileIndex);
        if (!tile || !tile->header || tile->dataSize == 0)
            continue;

        const uint64 tileRef = static_cast<uint64>(navMesh->getTileRef(tile));
        const uint32 dataSize = static_cast<uint32>(tile->dataSize);
        WriteNavBytes(outData, offset, &tileRef, sizeof(uint64));
        WriteNavBytes(outData, offset, &dataSize, sizeof(uint32));
        WriteNavBytes(outData, offset, tile->data, dataSize);
    }
    if (digestBytes)
    {
        const TiledBakeMemo memo{m_Impl->BakeTilesX, m_Impl->BakeMinY, m_Impl->BakeMaxY, m_Impl->BakeSettings};
        WriteNavBytes(outData, offset, kTiledBakeTag, sizeof(kTiledBakeTag));
        WriteNavBytes(outData, offset, &memo, sizeof(memo));
        WriteNavBytes(outData, offset, m_Impl->TileDigests.data(), digestBytes);
    }

    return true;
}

bool DetourNavMap::Deserialize(const uint8* data, uint32 size)
{
    if (!data || size == 0)
        return false;

    const size_t minHeaderSize = 4 + sizeof(uint32) + sizeof(dtNavMeshParams) + sizeof(uint32);
    if (size < minHeaderSize)
        return false;

    size_t offset = 0;
    const std::span bytes(data, size);

    // Check magic
    char magic[4];
    if (!ReadNavBytes(bytes, offset, magic, 4) || std::memcmp(magic, kSerializeMagic, 4) != 0)
    {
        // Try legacy single-tile format (no header, raw Detour tile data).
        // The old format wrote tile data directly with no magic/version.
        return DeserializeLegacy(data, size);
    }

    uint32 version = 0;
    if (!ReadNavBytes(bytes, offset, &version, sizeof(uint32)) || version != kSerializeVersion)
        return false;

    dtNavMeshParams meshParams;
    if (!ReadNavBytes(bytes, offset, &meshParams, sizeof(dtNavMeshParams)))
        return false;

    uint32 tileCount = 0;
    if (!ReadNavBytes(bytes, offset, &tileCount, sizeof(uint32)))
        return false;

    m_Impl = std::make_unique<Impl>();

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
        return false;

    dtStatus status = m_Impl->NavMesh->init(&meshParams);
    if (dtStatusFailed(status))
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    uint32 tilesLoaded = 0;
    for (uint32 tileIndex = 0; tileIndex < tileCount; ++tileIndex)
    {
        uint64 tileRef = 0;
        uint32 dataSize = 0;
        if (!ReadNavBytes(bytes, offset, &tileRef, sizeof(uint64)) || !ReadNavBytes(bytes, offset, &dataSize, sizeof(uint32)))
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }

        if (dataSize == 0 || offset + dataSize > size)
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }

        // Detour takes ownership of tile data, so allocate a copy
        uint8* tileData = static_cast<uint8*>(dtAlloc(dataSize, DT_ALLOC_PERM));
        if (!tileData)
        {
            dtFreeNavMesh(m_Impl->NavMesh);
            m_Impl->NavMesh = nullptr;
            return false;
        }
        std::memcpy(tileData, data + offset, dataSize);
        offset += dataSize;

        status = m_Impl->NavMesh->addTile(tileData, static_cast<int32>(dataSize),
                                            DT_TILE_FREE_DATA,
                                            static_cast<dtTileRef>(tileRef), nullptr);
        if (dtStatusFailed(status))
            dtFree(tileData);
        else
            ++tilesLoaded;
    }

    if (tileCount != 0 && tilesLoaded == 0)
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    const int32 gridTiles = meshParams.maxTiles;
    if (tilesLoaded == tileCount && gridTiles > 1 && gridTiles <= kMaxBakeTiles)
    {
        char tag[sizeof(kTiledBakeTag)];
        TiledBakeMemo memo;
        const size_t digestBytes = static_cast<size_t>(gridTiles) * sizeof(uint64);
        // The suffix is a bake hint. A file without the hint still contains a usable navmesh.
        if (ReadNavBytes(bytes, offset, tag, sizeof(tag)) && std::memcmp(tag, kTiledBakeTag, sizeof(tag)) == 0 &&
            ReadNavBytes(bytes, offset, &memo, sizeof(memo)) && digestBytes == size - offset &&
            memo.Columns > 0 && gridTiles % memo.Columns == 0 &&
            std::isfinite(memo.MinY) && std::isfinite(memo.MaxY) &&
            memo.MinY == meshParams.orig[1] && memo.MinY <= memo.MaxY)
        {
            m_Impl->TileDigests.resize(gridTiles);
            std::memcpy(m_Impl->TileDigests.data(), data + offset, digestBytes);
            m_Impl->BakeTilesX = memo.Columns;
            m_Impl->BakeMinY = memo.MinY;
            m_Impl->BakeMaxY = memo.MaxY;
            m_Impl->BakeSettings = memo.Settings;
        }
    }

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    status = m_Impl->NavQuery->init(m_Impl->NavMesh, 2048);
    if (dtStatusFailed(status))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->BuildDebugMesh();
    return true;
}

bool DetourNavMap::DeserializeLegacy(const uint8* data, uint32 size)
{
    m_Impl = std::make_unique<Impl>();

    m_Impl->NavMesh = dtAllocNavMesh();
    if (!m_Impl->NavMesh)
        return false;

    uint8* navData = static_cast<uint8*>(dtAlloc(size, DT_ALLOC_PERM));
    if (!navData)
    {
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }
    std::memcpy(navData, data, size);

    dtStatus status = m_Impl->NavMesh->init(navData, size, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        dtFree(navData);
        dtFreeNavMesh(m_Impl->NavMesh);
        m_Impl->NavMesh = nullptr;
        return false;
    }

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    status = m_Impl->NavQuery->init(m_Impl->NavMesh, 2048);
    if (dtStatusFailed(status))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->BuildDebugMesh();
    return true;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    return m_Impl ? static_cast<uint32>(m_Impl->DebugVertices.size() / 3) : 0;
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    return m_Impl ? static_cast<uint32>(m_Impl->DebugIndices.size() / 3) : 0;
}

const float32* DetourNavMap::GetDebugVertices() const
{
    return (m_Impl && !m_Impl->DebugVertices.empty()) ? m_Impl->DebugVertices.data() : nullptr;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    return (m_Impl && !m_Impl->DebugIndices.empty()) ? m_Impl->DebugIndices.data() : nullptr;
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return m_Impl ? m_Impl->NavMesh : nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return m_Impl ? m_Impl->NavQuery : nullptr;
}

#else // !GE_PATHFINDING_BACKEND_DETOUR

struct DetourNavMap::Impl
{
};

DetourNavMap::DetourNavMap()
    : m_Impl(std::make_unique<Impl>())
{
}

DetourNavMap::~DetourNavMap() = default;
DetourNavMap::DetourNavMap(DetourNavMap&&) noexcept = default;
DetourNavMap& DetourNavMap::operator=(DetourNavMap&&) noexcept = default;

bool DetourNavMap::Build(const NavMeshSettings& /*settings*/, const InputGeometry& /*geometry*/)
{
    return false;
}

bool DetourNavMap::IsBuilt() const
{
    return false;
}

PathStatus DetourNavMap::FindPath(const PathRequest& /*request*/, PathBuffer& /*pathBuffer*/, PathHandle& /*outPath*/) const
{
    return PathStatus::Failed;
}

bool DetourNavMap::IsPointNavigable(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*radius*/) const
{
    return false;
}

bool DetourNavMap::GetClosestNavigablePoint(float32 /*x*/, float32 /*y*/, float32 /*z*/, float32 /*searchRadius*/,
                                             float32& /*outX*/, float32& /*outY*/, float32& /*outZ*/) const
{
    return false;
}

bool DetourNavMap::Raycast(float32 /*startX*/, float32 /*startY*/, float32 /*startZ*/,
                            float32 /*endX*/, float32 /*endY*/, float32 /*endZ*/,
                            float32& /*hitX*/, float32& /*hitY*/, float32& /*hitZ*/) const
{
    return false;
}

bool DetourNavMap::Serialize(std::vector<uint8>& /*outData*/) const
{
    return false;
}

bool DetourNavMap::Deserialize(const uint8* /*data*/, uint32 /*size*/)
{
    return false;
}

uint32 DetourNavMap::GetDebugVertexCount() const
{
    return 0;
}

uint32 DetourNavMap::GetDebugTriangleCount() const
{
    return 0;
}

const float32* DetourNavMap::GetDebugVertices() const
{
    return nullptr;
}

const uint32* DetourNavMap::GetDebugIndices() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMesh() const
{
    return nullptr;
}

void* DetourNavMap::GetDetourNavMeshQuery() const
{
    return nullptr;
}

#endif // GE_PATHFINDING_BACKEND_DETOUR

} // namespace GameEngine::Pathfinding
