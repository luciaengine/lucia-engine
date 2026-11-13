#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{
// The hook is process-global; only this synchronous caller refuses allocations.
class ScopedDetourAllocationFailure
{
  public:
    explicit ScopedDetourAllocationFailure(uint32 failAt) : m_FailAt(failAt)
    {
        s_Current = this;
        dtAllocSetCustom(Allocate, Free);
    }

    ~ScopedDetourAllocationFailure()
    {
        dtAllocSetCustom(nullptr, nullptr);
        s_Current = nullptr;
    }

    ScopedDetourAllocationFailure(const ScopedDetourAllocationFailure&) = delete;
    ScopedDetourAllocationFailure& operator=(const ScopedDetourAllocationFailure&) = delete;
    bool DidFail() const { return m_Failed; }
    uint32 Calls() const { return m_Calls; }

  private:
    static void* Allocate(size_t bytes, dtAllocHint)
    {
        if (s_Current && ++s_Current->m_Calls == s_Current->m_FailAt)
        {
            s_Current->m_Failed = true;
            return nullptr;
        }
        return std::malloc(bytes);
    }

    static void Free(void* bytes) { std::free(bytes); }

    inline static thread_local ScopedDetourAllocationFailure* s_Current = nullptr;
    uint32 m_FailAt;
    uint32 m_Calls = 0;
    bool m_Failed = false;
};

void ExpectTileData(const std::vector<NavMeshTileData>& expected, const DetourNavMap& map)
{
    std::vector<NavMeshTileData> actual;
    map.CopyTiles(actual);
    EXPECT_EQ(actual.size(), expected.size());
    for (const auto& tile : expected)
    {
        const auto found = std::find_if(actual.begin(), actual.end(),
                                         [&tile](const auto& other) { return other.Key == tile.Key; });
        EXPECT_NE(found, actual.end());
        if (found != actual.end())
        {
            EXPECT_EQ(found->ContentHash, tile.ContentHash);
            EXPECT_EQ(found->PolygonCount, tile.PolygonCount);
            EXPECT_EQ(found->Data, tile.Data);
        }
    }
}

PathStatus CrossTilePath(const DetourNavMap& map)
{
    PathRequest request;
    request.StartX = 8.0f;
    request.StartZ = 8.0f;
    request.EndX = 48.0f;
    request.EndZ = 8.0f;
    PathBuffer paths;
    PathHandle path;
    return map.FindPath(request, paths, path);
}
} // namespace

TEST(DetourNavMapReplacement, AllocationRefusalKeepsTheLiveTileAndItsNeighborUntilRetrySucceeds)
{
    for (const uint32 failAt : {1u, 2u})
    {
        SCOPED_TRACE(failAt);
        NavMeshSettings settings;
        settings.CellSize = 0.5f;
        settings.AgentRadius = 0.5f;
        NavMeshChart chart;
        chart.Span = 2.0f * DetourNavMap::GetTileSize(settings);
        const NavTileKey key{chart.Id, 0, 0};
        const NavTileKey neighborKey{chart.Id, 1, 0};
        const std::array<float32, 12> vertices{-8.0f, 0.0f, -8.0f, 72.0f, 0.0f, -8.0f,
                                               -8.0f, 0.0f, 40.0f, 72.0f, 0.0f, 40.0f};
        const std::array<uint32, 6> indices{0, 2, 1, 1, 2, 3};
        DetourNavMap::InputGeometry geometry;
        geometry.Vertices = vertices.data();
        geometry.VertexCount = 4u;
        geometry.Indices = indices.data();
        geometry.TriangleCount = 2u;
        NavMeshTileData original;
        NavMeshTileData neighbor;
        NavMeshTileData replacement;
        std::string error;
        ASSERT_TRUE(DetourNavMap::BuildTile(settings, key, geometry, original, error)) << error;
        ASSERT_TRUE(DetourNavMap::BuildTile(settings, neighborKey, geometry, neighbor, error)) << error;
        auto raised = vertices;
        for (size_t vertex = 1; vertex < raised.size(); vertex += 3)
            raised[vertex] = 6.0f;
        geometry.Vertices = raised.data();
        ASSERT_TRUE(DetourNavMap::BuildTile(settings, key, geometry, replacement, error)) << error;
        ASSERT_FALSE(original.Data.empty());
        ASSERT_FALSE(neighbor.Data.empty());
        ASSERT_FALSE(replacement.Data.empty());
        original.ContentHash = 11u;
        neighbor.ContentHash = 22u;
        replacement.ContentHash = 33u;

        DetourNavMap map;
        ASSERT_TRUE(map.Initialize(settings, chart));
        ASSERT_TRUE(map.AddTile(original));
        ASSERT_TRUE(map.AddTile(neighbor));
        auto* mesh = static_cast<dtNavMesh*>(map.GetDetourNavMesh());
        ASSERT_NE(mesh, nullptr);
        const dtTileRef originalRef = mesh->getTileRefAt(0, 0, 0);
        const dtTileRef neighborRef = mesh->getTileRefAt(1, 0, 0);
        ASSERT_NE(originalRef, 0u);
        ASSERT_NE(neighborRef, 0u);
        ASSERT_TRUE(dtStatusSucceed(mesh->setPolyFlags(originalRef, 3u)));
        ASSERT_TRUE(dtStatusSucceed(mesh->setPolyArea(originalRef, 7u)));
        ASSERT_EQ(CrossTilePath(map), PathStatus::Complete);
        const uint32 triangles = map.GetDebugTriangleCount();
        const uint64 debugVersion = map.GetDebugMeshVersion();
        ASSERT_GT(triangles, 0u);
        std::vector<NavMeshTileData> before;
        map.CopyTiles(before);

        bool accepted;
        bool allocationFailed;
        uint32 allocationCalls;
        {
            const ScopedDetourAllocationFailure failure(failAt);
            accepted = map.AddTile(replacement, &error);
            allocationFailed = failure.DidFail();
            allocationCalls = failure.Calls();
        }
        EXPECT_TRUE(allocationFailed) << "the allocator refusal was not exercised";
        EXPECT_EQ(allocationCalls, failAt);
        EXPECT_FALSE(accepted);
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(map.GetDetourNavMesh(), mesh);
        EXPECT_EQ(mesh->getTileRefAt(0, 0, 0), originalRef);
        EXPECT_EQ(mesh->getTileRefAt(1, 0, 0), neighborRef);
        EXPECT_TRUE(mesh->isValidPolyRef(originalRef));
        EXPECT_TRUE(mesh->isValidPolyRef(neighborRef));
        unsigned short flags = 0;
        unsigned char area = 0;
        EXPECT_TRUE(dtStatusSucceed(mesh->getPolyFlags(originalRef, &flags)));
        EXPECT_TRUE(dtStatusSucceed(mesh->getPolyArea(originalRef, &area)));
        EXPECT_EQ(flags, 3u);
        EXPECT_EQ(area, 7u);
        EXPECT_EQ(CrossTilePath(map), PathStatus::Complete);
        EXPECT_EQ(map.GetDebugTriangleCount(), triangles);
        EXPECT_EQ(map.GetDebugMeshVersion(), debugVersion);
        ExpectTileData(before, map);

        ASSERT_TRUE(map.AddTile(replacement, &error)) << error;
        EXPECT_FALSE(mesh->isValidPolyRef(originalRef));
        EXPECT_EQ(mesh->getTileRefAt(1, 0, 0), neighborRef);
        EXPECT_TRUE(mesh->isValidPolyRef(neighborRef));
        EXPECT_FALSE(map.IsPointNavigable(8.0f, 0.0f, 8.0f, 0.5f));
        EXPECT_TRUE(map.IsPointNavigable(8.0f, 6.0f, 8.0f, 0.5f));
        EXPECT_NE(CrossTilePath(map), PathStatus::Complete);
        EXPECT_NE(map.GetDebugMeshVersion(), debugVersion);
        ExpectTileData({replacement, neighbor}, map);
    }
}
