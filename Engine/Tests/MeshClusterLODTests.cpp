#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"

#include "AssetCore/SharedFileRead.h"
#include "AssetCore/DepEdge.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/Parsers/ModelAssetParser.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODCut.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "ModelAssetFbxTestAccess.h"
#include "ClusterLODTestMesh.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Vector3.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace GameEngine
{
namespace
{

constexpr uint32 kGridWidth = Testing::kClusterLODGridWidth;
using Triangle = std::array<uint32, 3>;
using Edge = std::pair<uint32, uint32>;


uint32 PositionId(const Vertex& vertex)
{
    return static_cast<uint32>(vertex.Position[2]) * kGridWidth +
           static_cast<uint32>(vertex.Position[0]);
}

Vector<Triangle> OrientedTriangles(const Mesh& mesh)
{
    Vector<Triangle> triangles;
    for (size_t offset = 0; offset < mesh.Indices.size(); offset += 3)
    {
        Triangle triangle{PositionId(mesh.Vertices.at(mesh.Indices.at(offset))),
                          PositionId(mesh.Vertices.at(mesh.Indices.at(offset + 1))),
                          PositionId(mesh.Vertices.at(mesh.Indices.at(offset + 2)))};
        std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

std::set<Edge> BoundaryEdges(const Mesh& mesh)
{
    std::map<Edge, uint32> counts;
    for (const Triangle& triangle : OrientedTriangles(mesh))
    {
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32 first = triangle[corner];
            const uint32 second = triangle[(corner + 1) % 3];
            ++counts[{std::min(first, second), std::max(first, second)}];
        }
    }
    std::set<Edge> boundary;
    for (const auto& [edge, count] : counts)
        if (count == 1)
            boundary.insert(edge);
    return boundary;
}

MeshClusterLODConfig TestConfig()
{
    MeshClusterLODConfig config;
    config.MeshletsPerGroup = 4;
    return config;
}

void ExpectRefusal(const Mesh& mesh, const MeshClusterLODConfig& config)
{
    MeshClusterLODDag dag;
    dag.Groups.push_back({{}, 7.0f, 0.0f, 0, false});
    String error;
    EXPECT_FALSE(GenerateMeshClusterLODDag(mesh, config, dag, error));
    EXPECT_FALSE(error.empty());
    ASSERT_EQ(dag.Groups.size(), 1u);
    EXPECT_EQ(dag.Groups.front().Radius, 7.0f);
}

// Use counts of the position edges of the triangles.
std::map<Edge, uint32> EdgeUses(const Vector<Triangle>& triangles)
{
    std::map<Edge, uint32> uses;
    for (const Triangle& triangle : triangles)
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32 first = triangle[corner];
            const uint32 second = triangle[(corner + 1) % 3];
            ++uses[{std::min(first, second), std::max(first, second)}];
        }
    return uses;
}

// The edges not used by exactly two triangles, with their use counts.
std::map<Edge, uint32> BoundaryEdgeUses(const Vector<Triangle>& triangles)
{
    auto uses = EdgeUses(triangles);
    std::erase_if(uses, [](const auto& entry) { return entry.second == 2; });
    return uses;
}

// Counts the edges of a cut that leave a crack: an edge that only one triangle uses and that is
// not open in the source, an edge that more than two triangles use, or a shared edge that both
// triangles walk in the same direction.
size_t CountCutCracks(const Vector<Triangle>& cut, const std::set<Edge>& sourceBoundary)
{
    std::map<Edge, uint32> counts;
    std::map<Edge, int32> orientation;
    for (const Triangle& triangle : cut)
    {
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32 first = triangle[corner];
            const uint32 second = triangle[(corner + 1) % 3];
            const Edge edge{std::min(first, second), std::max(first, second)};
            ++counts[edge];
            orientation[edge] += first < second ? 1 : -1;
        }
    }
    size_t cracks = 0;
    for (const auto& [edge, count] : counts)
    {
        const bool open = count == 1 && !sourceBoundary.contains(edge);
        cracks += open || count > 2 || (count == 2 && orientation[edge] != 0) ? 1 : 0;
    }
    return cracks;
}

uint32 MaximumDepth(const MeshClusterLODDag& dag)
{
    uint32 depth = 0;
    for (const MeshClusterLODDagGroup& group : dag.Groups)
        depth = std::max(depth, group.Depth);
    return depth;
}

// The oriented triangles of the units of the DAG that `drawn` selects.
template <typename Selector>
Vector<Triangle> DagTriangles(const Mesh& source, const MeshClusterLODDag& dag, Selector&& drawn)
{
    Mesh selected;
    selected.Vertices = source.Vertices;
    for (const MeshClusterLODDagUnit& unit : dag.Units)
        if (drawn(unit))
            selected.Indices.insert(selected.Indices.end(), unit.Indices.begin(), unit.Indices.end());
    return OrientedTriangles(selected);
}

Vector<Triangle> LevelTriangles(const Mesh& source, const MeshClusterLODDag& dag, uint32 depth)
{
    return DagTriangles(source, dag, [&dag, depth](const MeshClusterLODDagUnit& unit)
        { return IsMeshClusterLODUnitInLevel(dag.Groups[unit.Group].Depth, dag.Groups[unit.Group].Terminal, depth); });
}

// The test grid with a smooth height field, so no simplified triangle is exactly collinear.
Mesh MakeCurvedGrid()
{
    Mesh mesh = Testing::MakeClusterLODGrid();
    for (Vertex& vertex : mesh.Vertices)
        vertex.Position[1] = 0.5f * std::sin(0.4f * vertex.Position[0]) * std::cos(0.3f * vertex.Position[2]);
    return mesh;
}

// Splits the grid along its middle column with duplicate wedges that `change` edits.
template <typename Change>
Mesh AddSeam(Mesh source, Change&& change)
{
    constexpr uint32 kSeamColumn = kGridWidth / 2;
    for (uint32 row = 0; row < kGridWidth; ++row)
    {
        Vertex duplicate = source.Vertices[row * kGridWidth + kSeamColumn];
        change(duplicate);
        source.Vertices.push_back(duplicate);
    }
    for (size_t offset = 0; offset < source.Indices.size(); offset += 3)
    {
        const bool rightSide = std::any_of(source.Indices.begin() + offset, source.Indices.begin() + offset + 3,
                                           [](uint32 index) { return index % kGridWidth > kSeamColumn; });
        if (!rightSide)
            continue;
        for (size_t corner = offset; corner < offset + 3; ++corner)
            if (source.Indices[corner] % kGridWidth == kSeamColumn)
                source.Indices[corner] = kGridWidth * kGridWidth + source.Indices[corner] / kGridWidth;
    }
    return source;
}

TEST(MeshClusterLOD, PartitionsEveryOrientedSourceTriangleExactlyOnceIntoLevelZero)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    ASSERT_GT(dag.Groups.size(), 1u);
    for (const MeshClusterLODDagUnit& unit : dag.Units)
        EXPECT_EQ(unit.Producer == kMeshClusterLODSourceGroup, dag.Groups[unit.Group].Depth == 0);
    EXPECT_EQ(DagTriangles(source, dag, [](const MeshClusterLODDagUnit& unit)
                           { return unit.Producer == kMeshClusterLODSourceGroup; }),
              OrientedTriangles(source));
    EXPECT_EQ(LevelTriangles(source, dag, 0), OrientedTriangles(source));
}

TEST(MeshClusterLOD, EveryUniformLevelCoversTheSourceWithoutCracksAndFewerTriangles)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = MakeCurvedGrid();
    const auto sourceBoundary = BoundaryEdges(source);
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    EXPECT_GE(MaximumDepth(dag), 2u) << "the DAG must reduce over more than one level";
    size_t previous = std::numeric_limits<size_t>::max();
    for (uint32 depth = 0; depth <= MaximumDepth(dag); ++depth)
    {
        SCOPED_TRACE(depth);
        const auto level = LevelTriangles(source, dag, depth);
        EXPECT_EQ(CountCutCracks(level, sourceBoundary), 0u);
        EXPECT_EQ(BoundaryEdgeUses(level), BoundaryEdgeUses(OrientedTriangles(source)));
        EXPECT_LT(level.size(), previous);
        previous = level.size();
    }
    EXPECT_LT(previous * 8, source.Indices.size() / 3) << "the coarsest level must keep under an eighth";
}

TEST(MeshClusterLOD, EmitsOnlyPositiveAreaTrianglesOnThePlanarGrid)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    for (const MeshClusterLODDagUnit& unit : dag.Units)
    {
        for (size_t offset = 0; offset < unit.Indices.size(); offset += 3)
        {
            const auto& first = source.Vertices.at(unit.Indices[offset]).Position;
            const auto& second = source.Vertices.at(unit.Indices[offset + 1]).Position;
            const auto& third = source.Vertices.at(unit.Indices[offset + 2]).Position;
            const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
            const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
            EXPECT_GT(Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared(), 0.0f);
        }
    }
}

TEST(MeshClusterLOD, ErrorAndSpheresGrowUpTheDag)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = MakeCurvedGrid();
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    size_t edges = 0;
    for (const MeshClusterLODDagUnit& unit : dag.Units)
    {
        if (unit.Producer == kMeshClusterLODSourceGroup)
            continue;
        const auto& group = dag.Groups[unit.Group];
        const auto& producer = dag.Groups[unit.Producer];
        SCOPED_TRACE(std::format("group {} producer {}", unit.Group, unit.Producer));
        EXPECT_FALSE(producer.Terminal);
        EXPECT_EQ(group.Depth, producer.Depth + 1);
        if (!group.Terminal)
            EXPECT_GE(group.Error, producer.Error);
        const float distance = (group.Center - producer.Center).Length();
        EXPECT_LE(distance + producer.Radius, group.Radius * 1.001f + 1e-4f);
        ++edges;
    }
    EXPECT_GT(edges, 0u);
    EXPECT_TRUE(std::any_of(dag.Groups.begin(), dag.Groups.end(),
                            [](const MeshClusterLODDagGroup& group) { return !group.Terminal && group.Error > 0.0f; }));
}

TEST(MeshClusterLOD, RepeatsTheSameDag)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = MakeCurvedGrid();
    MeshClusterLODDag first;
    MeshClusterLODDag second;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), first, error)) << error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), second, error)) << error;
    ASSERT_EQ(first.Groups.size(), second.Groups.size());
    ASSERT_EQ(first.Units.size(), second.Units.size());
    for (size_t group = 0; group < first.Groups.size(); ++group)
    {
        EXPECT_EQ(first.Groups[group].Error, second.Groups[group].Error);
        EXPECT_EQ(first.Groups[group].Radius, second.Groups[group].Radius);
    }
    for (size_t unit = 0; unit < first.Units.size(); ++unit)
        EXPECT_EQ(first.Units[unit].Indices, second.Units[unit].Indices);
}

TEST(MeshClusterLOD, ConvertsNonzeroSimplificationErrorToLocalUnits)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    for (Vertex& vertex : source.Vertices)
        vertex.Position[1] = 4.0f * std::sin(vertex.Position[0] * 0.2f) * std::sin(vertex.Position[2] * 0.15f);
    MeshClusterLODDag original;
    MeshClusterLODDag scaled;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), original, error)) << error;
    constexpr float kScale = 4.0f;
    for (Vertex& vertex : source.Vertices)
        for (float& component : vertex.Position)
            component *= kScale;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), scaled, error)) << error;
    ASSERT_EQ(original.Groups.size(), scaled.Groups.size());
    size_t compared = 0;
    for (size_t group = 0; group < original.Groups.size(); ++group)
    {
        if (original.Groups[group].Terminal || original.Groups[group].Error <= 0.0f)
            continue;
        EXPECT_NEAR(scaled.Groups[group].Error, original.Groups[group].Error * kScale,
                    original.Groups[group].Error * kScale * 0.01f);
        ++compared;
    }
    EXPECT_GT(compared, 0u);
}

TEST(MeshClusterLOD, KeepsEachSideOfADuplicatePositionWedgeAtEveryLevel)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = AddSeam(MakeCurvedGrid(), [](Vertex& vertex)
    {
        vertex.TexCoords[0] += 1.0f;
        vertex.Normal[0] = 0.25f;
    });
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    constexpr float kSeam = static_cast<float>(kGridWidth / 2);
    for (const MeshClusterLODDagUnit& unit : dag.Units)
    {
        for (size_t offset = 0; offset < unit.Indices.size(); offset += 3)
        {
            float centroid = 0.0f;
            for (size_t corner = 0; corner < 3; ++corner)
                centroid += source.Vertices[unit.Indices[offset + corner]].Position[0] / 3.0f;
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const Vertex& vertex = source.Vertices[unit.Indices[offset + corner]];
                if (vertex.Position[0] == kSeam)
                    EXPECT_EQ(vertex.Normal[0] != 0.0f, centroid > kSeam) << "a wedge crossed the seam";
            }
        }
    }
}

TEST(MeshClusterLOD, CollapsesAlongAUvSeamWithoutOpeningIt)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = AddSeam(MakeCurvedGrid(), [](Vertex& vertex) { vertex.TexCoords[0] += 1.0f; });
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    const auto sourceBorder = BoundaryEdges(source);
    std::set<uint32> seamRows;
    for (uint32 depth = 0; depth <= MaximumDepth(dag); ++depth)
        EXPECT_EQ(CountCutCracks(LevelTriangles(source, dag, depth), sourceBorder), 0u) << depth;
    for (const Triangle& triangle : LevelTriangles(source, dag, 1))
        for (const uint32 position : triangle)
            if (position % kGridWidth == kGridWidth / 2)
                seamRows.insert(position / kGridWidth);
    EXPECT_LT(seamRows.size(), kGridWidth);
}

TEST(MeshClusterLOD, KeepsAMirroredUvSeamWithoutMixingTangentHandedness)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    // The right half mirrors the UVs: its seam wedges differ from the left half only in handedness.
    Mesh grid = MakeCurvedGrid();
    for (Vertex& vertex : grid.Vertices)
        if (vertex.Position[0] > kGridWidth / 2)
            vertex.Tangent[3] = -1.0f;
    const Mesh source = AddSeam(grid, [](Vertex& vertex) { vertex.Tangent[3] = -1.0f; });
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    for (const MeshClusterLODDagUnit& unit : dag.Units)
        for (size_t offset = 0; offset < unit.Indices.size(); offset += 3)
        {
            const float handedness = source.Vertices[unit.Indices[offset]].Tangent[3];
            EXPECT_EQ(source.Vertices[unit.Indices[offset + 1]].Tangent[3], handedness);
            EXPECT_EQ(source.Vertices[unit.Indices[offset + 2]].Tangent[3], handedness);
        }
    for (uint32 depth = 0; depth <= MaximumDepth(dag); ++depth)
        EXPECT_EQ(CountCutCracks(LevelTriangles(source, dag, depth), BoundaryEdges(source)), 0u) << depth;
}

TEST(MeshClusterLOD, ClustersVerticesWhenSeamsStopTheAttributeSimplifier)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    // Every triangle has its own UV island, so every vertex is on a UV seam.
    const Mesh grid = MakeCurvedGrid();
    Mesh source = grid;
    source.Vertices.clear();
    source.Indices.clear();
    for (size_t corner = 0; corner < grid.Indices.size(); ++corner)
    {
        Vertex vertex = grid.Vertices[grid.Indices[corner]];
        vertex.TexCoords[0] = static_cast<float>(corner % 3);
        vertex.TexCoords[1] = static_cast<float>(corner / 3);
        source.Vertices.push_back(vertex);
        source.Indices.push_back(static_cast<uint32>(corner));
    }
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
    const auto level = LevelTriangles(source, dag, 1);
    EXPECT_LT(level.size() * 4, grid.Indices.size() / 3 * 3);
    EXPECT_EQ(CountCutCracks(level, BoundaryEdges(grid)), 0u);
}

TEST(MeshClusterLOD, RejectsUnsupportedRawStreamsWithoutPublishing)
{
    for (uint32 stream = 0; stream < 8; ++stream)
    {
        SCOPED_TRACE(stream);
        Mesh source = Testing::MakeClusterLODGrid();
        switch (stream)
        {
        case 0: source.Color0.push_back(1.0f); break;
        case 1: source.TexCoords1.push_back(1.0f); break;
        case 2: source.ExtraTexCoords.emplace_back(); break;
        case 3: source.Joints0.push_back(0); break;
        case 4: source.Weights1.push_back(1.0f); break;
        case 5: source.Skinned = true; break;
        case 6: source.MorphTargets.emplace_back(); break;
        case 7: source.AuthoredLODs = true; break;
        }
        ExpectRefusal(source, TestConfig());
    }
}

TEST(MeshClusterLOD, RejectsMalformedGeometryWithoutPublishing)
{
    for (uint32 fault = 0; fault < 6; ++fault)
    {
        SCOPED_TRACE(fault);
        Mesh source = Testing::MakeClusterLODGrid();
        switch (fault)
        {
        case 0: source.Indices.pop_back(); break;
        case 1: source.Indices.front() = static_cast<uint32>(source.Vertices.size()); break;
        case 2: source.Indices[1] = source.Indices[0]; break;
        case 3: source.Vertices[0].Position[0] = std::numeric_limits<float>::infinity(); break;
        case 4: source.Vertices[0].Normal[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 5: source.Vertices[0].Position[0] = std::numeric_limits<float>::max(); break;
        }
        ExpectRefusal(source, TestConfig());
    }
}

TEST(MeshClusterLOD, AcceptsNonManifoldSourcesAndKeepsTheirSpecialEdgesAtEveryLevel)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const uint32 middle = (kGridWidth / 2) * kGridWidth + kGridWidth / 2;
    for (uint32 fault = 0; fault < 3; ++fault)
    {
        SCOPED_TRACE(fault);
        Mesh source = MakeCurvedGrid();
        switch (fault)
        {
        case 0: // A duplicate face.
            source.Indices.insert(source.Indices.end(), source.Indices.begin(), source.Indices.begin() + 3);
            break;
        case 1: // One triangle with the opposite winding.
            std::swap(source.Indices[3000], source.Indices[3001]);
            break;
        case 2: // A third triangle on an interior edge.
            source.Indices.insert(source.Indices.end(), {middle, middle + 1, middle + 3 * kGridWidth});
            break;
        }
        MeshClusterLODDag dag;
        String error;
        ASSERT_TRUE(GenerateMeshClusterLODDag(source, TestConfig(), dag, error)) << error;
        EXPECT_GE(MaximumDepth(dag), 2u);
        const auto sourceBorder = BoundaryEdgeUses(OrientedTriangles(source));
        for (uint32 depth = 0; depth <= MaximumDepth(dag); ++depth)
            EXPECT_EQ(BoundaryEdgeUses(LevelTriangles(source, dag, depth)), sourceBorder) << depth;
    }
}

TEST(MeshClusterLOD, EnforcesGeometryScratchMetadataAndGroupLimits)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    auto config = TestConfig();
    config.MaximumGeometryBytes = 1;
    ExpectRefusal(source, config);
    config = TestConfig();
    config.MaximumScratchBytes = 1;
    ExpectRefusal(source, config);
    config = TestConfig();
    config.MaximumMetadataBytes = 1;
    ExpectRefusal(source, config);
    config = TestConfig();
    config.MaximumGroups = 1;
    ExpectRefusal(source, config);
    config = TestConfig();
    config.MeshletTriangles = 5;
    ExpectRefusal(source, config);
}

TEST(MeshClusterLOD, CancellationRetainsTheUnpublishedOutput)
{
    auto cancelled = std::make_shared<std::atomic<bool>>(true);
    ScopedAssetDecodeCancellation scope(cancelled);
    ExpectRefusal(Testing::MakeClusterLODGrid(), TestConfig());
}

TEST(MeshClusterLOD, RefusesOutputThatExceedsAnAdmittedSourcePayloadLimit)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    auto config = TestConfig();
    MeshClusterLODDag dag;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODDag(source, config, dag, error)) << error;
    uint64 unitIndices = 0;
    for (const MeshClusterLODDagUnit& unit : dag.Units)
        unitIndices += unit.Indices.size();
    const uint64 sourceBytes = source.Vertices.size() * sizeof(Vertex) + source.Indices.size() * sizeof(uint32);
    ASSERT_GT(source.Vertices.size() * sizeof(Vertex) + unitIndices * sizeof(uint32), sourceBytes);
    config.MaximumGeometryBytes = sourceBytes;
    ExpectRefusal(source, config);
}

class MeshClusterLODAssetTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!IsMeshLODGenerationAvailable())
            GTEST_SKIP() << "meshoptimizer not compiled in";
        Mesh mesh = Testing::MakeClusterLODGrid();
        mesh.MaterialIndex = 0;
        m_Source.SetMeshesForTest({std::move(mesh)});
        auto settings = m_PreviousLODSettings;
        settings.AutoGenerateOnImport = true;
        SetLODImportSettings(settings);
    }

    void TearDown() override { SetLODImportSettings(m_PreviousLODSettings); }

    bool Cook(const std::filesystem::path& path)
    {
        return CookMeshClusterLODAsset(m_Source, path, TestConfig(), m_Error);
    }

    TestUtils::ScopedTempDir m_Temporary{TestUtils::MakeUniqueTempDirectory("cluster_lod")};
    ModelAsset m_Source{GUID::Generate(), m_Temporary.Path() / "missing-source.obj"};
    LODImportSettings m_PreviousLODSettings = GetLODImportSettings();
    String m_Error;
};

// Every unit of the cooked model as a standalone mesh: streamed units from their pages,
// resident units from their range of the coarse chain.
bool LoadUnits(const std::filesystem::path& path, const ModelAsset& model, Vector<Mesh>& units, String& error)
{
    const auto& manifest = *model.GetClusterLODManifest();
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, manifest));
    units.assign(manifest.Units.size(), {});
    for (size_t index = 0; index < manifest.Units.size(); ++index)
    {
        const auto& unit = manifest.Units[index];
        if (!unit.IsStreamed())
        {
            units[index] = ExtractMeshClusterLODResidentUnit(model.GetMesh(unit.Mesh), unit);
            continue;
        }
        Vector<uint8> bytes(static_cast<size_t>(unit.Bytes));
        if (!store.SeekTo(unit.Offset) || store.Read(bytes.data(), bytes.size()) != static_cast<int64>(bytes.size()) ||
            !DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(unit.Mesh), units[index], error))
            return false;
    }
    return true;
}

Vector<Triangle> CutTriangles(const MeshClusterLODManifest& manifest, const Vector<Mesh>& units,
                              std::span<const Engine::Renderer::ClusterLODCutDecision> decisions)
{
    Vector<Triangle> triangles;
    for (uint32 unit = 0; unit < manifest.Units.size(); ++unit)
        if (Engine::Renderer::IsClusterLODUnitDrawn(manifest, unit, decisions))
        {
            const auto unitTriangles = OrientedTriangles(units[unit]);
            triangles.insert(triangles.end(), unitTriangles.begin(), unitTriangles.end());
        }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

TEST_F(MeshClusterLODAssetTest, LoadsTheCoarseAssetWithoutTheSourceOrFineStore)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    ASSERT_EQ(model.GetMeshCount(), 1u);
    ASSERT_EQ(manifest->Meshes.size(), 1u);
    EXPECT_GE(manifest->Meshes[0].ResidentDepth, 1u);
    EXPECT_TRUE(std::any_of(manifest->Units.begin(), manifest->Units.end(),
                            [](const auto& unit) { return unit.IsStreamed(); }));
    ASSERT_TRUE(std::filesystem::remove(ResolveMeshClusterLODStore(path, {}, *manifest)));
    EXPECT_FALSE(std::filesystem::exists(m_Source.GetPath()));
    ModelAsset coarseOnly(GUID::Generate(), path);
    ASSERT_TRUE(coarseOnly.Load());
    coarseOnly.PostLoad();
    ASSERT_EQ(coarseOnly.GetMeshCount(), 1u);
    EXPECT_FALSE(coarseOnly.GetMesh(0).Indices.empty());
    EXPECT_EQ(coarseOnly.GetMesh(0).Name, m_Source.GetMesh(0).Name);
}

// LOD0 is never coarser than its error budget: a mesh whose levels simplify badly keeps a finer
// LOD0, here its source, and needs no streamed unit.
TEST_F(MeshClusterLODAssetTest, TheErrorBudgetKeepsAFinerResidentCut)
{
    Mesh curved = MakeCurvedGrid();
    curved.MaterialIndex = 0;
    m_Source.SetMeshesForTest({curved});
    const auto streamedUnits = [](const MeshClusterLODManifest& manifest)
    {
        return std::count_if(manifest.Units.begin(), manifest.Units.end(), [](const auto& unit) { return unit.IsStreamed(); });
    };
    const auto bounded = m_Temporary.Path() / "bounded.gecluster";
    ASSERT_TRUE(Cook(bounded)) << m_Error;
    ModelAsset model(GUID::Generate(), bounded);
    ASSERT_TRUE(model.Load());
    EXPECT_GE(model.GetClusterLODManifest()->Meshes[0].ResidentDepth, 1u);
    EXPECT_GT(streamedUnits(*model.GetClusterLODManifest()), 0);

    auto config = TestConfig();
    config.ResidentRelativeError = 0.0f;
    const auto exact = m_Temporary.Path() / "exact.gecluster";
    ASSERT_TRUE(CookMeshClusterLODAsset(m_Source, exact, config, m_Error)) << m_Error;
    ModelAsset source(GUID::Generate(), exact);
    ASSERT_TRUE(source.Load());
    EXPECT_EQ(source.GetClusterLODManifest()->Meshes[0].ResidentDepth, 0u);
    EXPECT_EQ(streamedUnits(*source.GetClusterLODManifest()), 0);
    EXPECT_EQ(OrientedTriangles(source.GetMesh(0)), OrientedTriangles(curved));
}

// The cooked DAG stops at the resident level: every uniform cut under it covers the source, and
// the cut at the resident level is LOD0.
TEST_F(MeshClusterLODAssetTest, EveryUniformLevelOfTheCookedModelEndsAtLodZero)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto& manifest = *model.GetClusterLODManifest();
    Vector<Mesh> units;
    ASSERT_TRUE(LoadUnits(path, model, units, m_Error)) << m_Error;
    const auto& range = manifest.Meshes[0];
    for (const auto& group : manifest.Groups)
    {
        EXPECT_LE(group.Depth, range.ResidentDepth);
        EXPECT_TRUE(group.Depth < range.ResidentDepth || group.Terminal);
    }
    for (uint32 depth = 0; depth <= range.ResidentDepth; ++depth)
    {
        SCOPED_TRACE(depth);
        Vector<Engine::Renderer::ClusterLODCutDecision> decisions(range.GroupCount);
        for (uint32 group = 0; group < range.GroupCount; ++group)
            decisions[group] = manifest.Groups[group].Terminal || manifest.Groups[group].Depth >= depth
                ? Engine::Renderer::ClusterLODCutDecision::Refined
                : Engine::Renderer::ClusterLODCutDecision::Coarse;
        const auto cut = CutTriangles(manifest, units, decisions);
        if (depth == 0)
            EXPECT_EQ(cut, OrientedTriangles(m_Source.GetMesh(0)));
        if (depth == range.ResidentDepth)
            EXPECT_EQ(cut, OrientedTriangles(model.GetMesh(0)));
    }
    EXPECT_TRUE(model.GetMesh(0).ExtraLODs.empty()) << "the far chain is generated at load, not cooked";
}

TEST_F(MeshClusterLODAssetTest, StoresEachFinePageLosslesslyInLessThanHalfItsRawSize)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    MeshClusterLODDag dag;
    ASSERT_TRUE(GenerateMeshClusterLODDag(m_Source.GetMesh(0), TestConfig(), dag, m_Error)) << m_Error;
    ASSERT_EQ(dag.Units.size(), manifest->Units.size());
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    uint64 storedBytes = 0;
    for (size_t index = 0; index < dag.Units.size(); ++index)
    {
        const auto& unit = manifest->Units[index];
        if (!unit.IsStreamed())
            continue;
        Vector<uint32> remap(m_Source.GetMesh(0).Vertices.size(), kMeshClusterLODUnusedVertex);
        Vector<uint32> sourceVertices;
        const Mesh expected = CompactMeshClusterLODTriangles(m_Source.GetMesh(0), dag.Units[index].Indices, remap,
                                                             sourceVertices);
        Vector<uint8> bytes(static_cast<size_t>(unit.Bytes));
        ASSERT_TRUE(store.SeekTo(unit.Offset));
        ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
        Mesh fine;
        ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(0), fine, m_Error)) << m_Error;
        ASSERT_EQ(fine.Vertices.size(), expected.Vertices.size());
        EXPECT_EQ(std::memcmp(fine.Vertices.data(), expected.Vertices.data(), fine.Vertices.size() * sizeof(Vertex)), 0);
        EXPECT_EQ(OrientedTriangles(fine), OrientedTriangles(expected));
        const uint64 rawBytes = expected.Vertices.size() * sizeof(Vertex) + expected.Indices.size() * sizeof(uint32);
        EXPECT_LT(unit.Bytes * 2, rawBytes);
        storedBytes += unit.Bytes;
    }
    EXPECT_GT(storedBytes, 0u);
    EXPECT_EQ(storedBytes, manifest->StoreBytes);
}

const MeshClusterLODManifest::Unit& FirstStreamedUnit(const MeshClusterLODManifest& manifest)
{
    return *std::find_if(manifest.Units.begin(), manifest.Units.end(), [](const auto& unit) { return unit.IsStreamed(); });
}

TEST_F(MeshClusterLODAssetTest, RefusesAFinePageWithACorruptStreamHeaderEvenWithAMatchingChecksum)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    auto unit = FirstStreamedUnit(*manifest);
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    Vector<uint8> bytes(static_cast<size_t>(unit.Bytes));
    ASSERT_TRUE(store.SeekTo(unit.Offset));
    ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    // The first byte of each meshopt stream is its header.
    for (const size_t header : {size_t{0}, static_cast<size_t>(unit.EncodedVertexBytes)})
    {
        SCOPED_TRACE(std::format("stream header at byte {}", header));
        const uint8 original = bytes[header];
        bytes[header] = 0;
        unit.ContentHash = Hashing::Fnv1a64(bytes.data(), bytes.size());
        Mesh output;
        output.Name = "Previous valid mesh";
        EXPECT_FALSE(DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(0), output, m_Error));
        EXPECT_EQ(output.Name, "Previous valid mesh");
        bytes[header] = original;
    }
    unit.ContentHash = Hashing::Fnv1a64(bytes.data(), bytes.size());
    for (const uint32 split : {static_cast<uint32>(unit.Bytes), static_cast<uint32>(unit.Bytes) + 1u})
    {
        auto oversized = unit;
        oversized.EncodedVertexBytes = split;
        Mesh output;
        EXPECT_FALSE(DecodeMeshClusterLODFinePage(bytes, oversized, model.GetMesh(0), output, m_Error));
    }
    Mesh output;
    EXPECT_TRUE(DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(0), output, m_Error)) << m_Error;
}

// The projected error of every group of the cooked model from `eye`, over the refine threshold
// of `budgetPx`, as the runtime measures it for an identity transform.
void MeasureCut(const MeshClusterLODManifest& manifest, const Mathematics::Vector3& eye, float budgetPx,
                std::span<const Engine::Renderer::ClusterLODCutDecision> previous,
                Vector<Engine::Renderer::ClusterLODCutGroup>& groups)
{
    // The feature bounds the instance scale by the Frobenius norm of its basis: sqrt(3) for identity.
    const double scale = std::sqrt(3.0);
    constexpr double kVerticalFieldOfView = 1.0471976; // 60 degrees
    const double projectionScaleY = 1.0 / std::tan(0.5 * kVerticalFieldOfView);
    constexpr uint32 kViewportHeight = 1080;
    groups.assign(manifest.Groups.size(), {});
    for (uint32 index = 0; index < manifest.Groups.size(); ++index)
    {
        const auto& group = manifest.Groups[index];
        auto& state = groups[index];
        state.WasRefined = Engine::Renderer::IsClusterLODRefined(previous[index]);
        const double pixels = Rendering::ClusterGroupErrorPx(group.Error, scale, (group.Center - eye).Length(),
                                                             group.Radius * scale, projectionScaleY, kViewportHeight,
                                                             0.0f, false);
        state.Demand = pixels / (budgetPx * (state.WasRefined ? Rendering::kClusterKeepFineBudgetScale
                                                              : Rendering::kClusterRefineBudgetScale));
    }
}

// The readiness of every group when exactly the units `ready` selects can draw.
void SetReadiness(const MeshClusterLODManifest& manifest, const Vector<bool>& ready,
                  Vector<Engine::Renderer::ClusterLODCutGroup>& groups)
{
    for (uint32 index = 0; index < manifest.Groups.size(); ++index)
    {
        const auto& group = manifest.Groups[index];
        groups[index].UnitsReady = std::all_of(manifest.Units.begin() + group.FirstUnit,
                                               manifest.Units.begin() + group.FirstUnit + group.UnitCount,
                                               [&](const auto& unit) { return ready[&unit - manifest.Units.data()]; });
        groups[index].ProducedReady = std::all_of(manifest.ProducedUnits.begin() + group.FirstProduced,
                                                  manifest.ProducedUnits.begin() + group.FirstProduced + group.ProducedCount,
                                                  [&](uint32 unit) { return ready[unit]; });
    }
}

// Selects cuts of the cooked model with the runtime function from several cameras, pixel budgets
// and triangle budgets, then moves the camera with some units not yet drawable. No cut may open
// an edge that is closed in the source, at the engine's group size and at smaller ones.
TEST_F(MeshClusterLODAssetTest, EverySelectedCutOfTheCookedModelIsCrackFree)
{
    using Engine::Renderer::ClusterLODCutDecision;
    Mesh curved = MakeCurvedGrid();
    curved.MaterialIndex = 0;
    for (Vertex& vertex : curved.Vertices)
        vertex.Position[1] *= 4.0f;
    m_Source.SetMeshesForTest({curved});
    const auto sourceBoundary = BoundaryEdges(curved);
    const std::array<Mathematics::Vector3, 5> eyes{{
        {24.0f, 3.0f, 24.0f}, {-6.0f, 4.0f, -6.0f}, {60.0f, 10.0f, 24.0f}, {10.0f, 1.5f, 40.0f}, {24.0f, 80.0f, -40.0f}}};
    const std::array<float, 4> budgetsPx{0.25f, 1.0f, 4.0f, 16.0f};
    for (const uint32 groupSize : {4u, 16u, MeshClusterLODConfig{}.MeshletsPerGroup, 32u})
    {
        SCOPED_TRACE(std::format("group size {}", groupSize));
        auto config = TestConfig();
        config.MeshletsPerGroup = groupSize;
        const auto path = m_Temporary.Path() / std::format("cracks{}.gecluster", groupSize);
        ASSERT_TRUE(CookMeshClusterLODAsset(m_Source, path, config, m_Error)) << m_Error;
        ModelAsset model(GUID::Generate(), path);
        ASSERT_TRUE(model.Load());
        const auto& manifest = *model.GetClusterLODManifest();
        Vector<Mesh> units;
        ASSERT_TRUE(LoadUnits(path, model, units, m_Error)) << m_Error;
        const Vector<bool> allReady(manifest.Units.size(), true);
        Vector<Engine::Renderer::ClusterLODCutGroup> groups;
        Vector<ClusterLODCutDecision> previous(manifest.Groups.size(), ClusterLODCutDecision::Coarse);
        Vector<ClusterLODCutDecision> decisions(manifest.Groups.size());
        std::set<size_t> distinctCuts;
        uint32 lcg = 12345;
        for (const auto& eye : eyes)
        {
            for (const float budgetPx : budgetsPx)
            {
                SCOPED_TRACE(std::format("eye {},{},{} budget {}", eye.x, eye.y, eye.z, budgetPx));
                // A first frame from nothing, with every unit drawable and no triangle budget.
                std::fill(previous.begin(), previous.end(), ClusterLODCutDecision::Coarse);
                MeasureCut(manifest, eye, budgetPx, previous, groups);
                SetReadiness(manifest, allReady, groups);
                Engine::Renderer::ClusterLODCutInstance instance{&manifest, 0, groups, decisions};
                const uint64 drawn = Engine::Renderer::SelectClusterLODCuts({&instance, 1}, 0, 0);
                const auto cut = CutTriangles(manifest, units, decisions);
                EXPECT_EQ(CountCutCracks(cut, sourceBoundary), 0u);
                EXPECT_EQ(drawn, cut.size());
                distinctCuts.insert(cut.size());
                // A triangle budget at half the free cut.
                const uint64 budget = std::max<uint64>(1, drawn / 2);
                const uint64 limited = Engine::Renderer::SelectClusterLODCuts({&instance, 1}, budget, 0);
                const auto limitedCut = CutTriangles(manifest, units, decisions);
                EXPECT_EQ(CountCutCracks(limitedCut, sourceBoundary), 0u);
                EXPECT_EQ(limited, limitedCut.size());
                EXPECT_LE(limited, drawn);
                // The next frame from another eye: the units of the last cut draw, a third of the
                // others are not ready yet.
                previous = decisions;
                Vector<bool> ready(manifest.Units.size());
                for (uint32 unit = 0; unit < manifest.Units.size(); ++unit)
                {
                    lcg = lcg * 1664525u + 1013904223u;
                    ready[unit] = Engine::Renderer::IsClusterLODUnitDrawn(manifest, unit, previous) || (lcg >> 16) % 3 != 0;
                }
                const auto& next = eyes[(&eye - eyes.data() + 1) % eyes.size()];
                MeasureCut(manifest, next, budgetPx, previous, groups);
                SetReadiness(manifest, ready, groups);
                Engine::Renderer::SelectClusterLODCuts({&instance, 1}, 0, 0);
                const auto moved = CutTriangles(manifest, units, decisions);
                EXPECT_EQ(CountCutCracks(moved, sourceBoundary), 0u);
                for (uint32 unit = 0; unit < manifest.Units.size(); ++unit)
                    if (Engine::Renderer::IsClusterLODUnitDrawn(manifest, unit, decisions))
                        EXPECT_TRUE(ready[unit]) << "a unit that cannot draw was selected";
            }
        }
        // The 49 x 49 grid has few groups at the larger group sizes.
        EXPECT_GE(distinctCuts.size(), groupSize == 4 ? 6u : 2u) << "the cameras and budgets must select different cuts";
    }
}

TEST_F(MeshClusterLODAssetTest, RejectsTruncatedAndCorruptedAssetsTransactionally)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    Vector<uint8> bytes;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, bytes));
    MeshClusterLODAssetData output;
    output.Manifest.Incarnation = 77;
    for (size_t length = 0; length < 24; ++length)
    {
        EXPECT_FALSE(DecodeMeshClusterLODAsset(std::span(bytes).first(length), output, m_Error));
        EXPECT_EQ(output.Manifest.Incarnation, 77u);
    }
    for (size_t offset : {size_t{0}, size_t{12}, bytes.size() / 2, bytes.size() - 1})
    {
        bytes[offset] ^= 0x80;
        EXPECT_FALSE(DecodeMeshClusterLODAsset(bytes, output, m_Error));
        EXPECT_EQ(output.Manifest.Incarnation, 77u);
        bytes[offset] ^= 0x80;
    }
    ASSERT_TRUE(DecodeMeshClusterLODAsset(bytes, output, m_Error)) << m_Error;
    EXPECT_NE(output.Manifest.Incarnation, 77u);
}

TEST_F(MeshClusterLODAssetTest, RejectsAChangedFinePageWithoutReplacingThePreviousMesh)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    const auto& unit = FirstStreamedUnit(*manifest);
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    Vector<uint8> bytes(static_cast<size_t>(unit.Bytes));
    ASSERT_TRUE(store.SeekTo(unit.Offset));
    ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    bytes.back() ^= 0x80;
    Mesh output;
    output.Name = "Previous valid mesh";
    EXPECT_FALSE(DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(0), output, m_Error));
    EXPECT_EQ(output.Name, "Previous valid mesh");
    bytes.back() ^= 0x80;
    ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, unit, model.GetMesh(0), output, m_Error)) << m_Error;
}

TEST_F(MeshClusterLODAssetTest, RefusesChangedIncarnationsBeforeHotReloadSwapsThePayload)
{
    const auto firstPath = m_Temporary.Path() / "first.gecluster";
    const auto secondPath = m_Temporary.Path() / "second.gecluster";
    ASSERT_TRUE(Cook(firstPath)) << m_Error;
    ModelAsset live(GUID::Generate(), firstPath);
    ASSERT_TRUE(live.Load());
    const auto original = live.GetClusterLODManifest();
    Mesh changed = m_Source.GetMesh(0);
    for (Vertex& vertex : changed.Vertices)
        vertex.Normal[0] = 0.1f;
    m_Source.SetMeshesForTest({std::move(changed)});
    ASSERT_TRUE(Cook(secondPath)) << m_Error;
    ModelAsset staged(live.GetGUID(), secondPath);
    ASSERT_TRUE(staged.Load());
    ASSERT_EQ(live.GetMeshCount(), staged.GetMeshCount());
    ASSERT_NE(original->Incarnation, staged.GetClusterLODManifest()->Incarnation);
    EXPECT_FALSE(live.AdoptReload(staged));
    EXPECT_EQ(live.GetClusterLODManifest(), original);
    EXPECT_FLOAT_EQ(live.GetMesh(0).Vertices.front().Normal[0], 0.0f);
    EXPECT_FLOAT_EQ(staged.GetMesh(0).Vertices.front().Normal[0], 0.1f);
    std::filesystem::copy_file(secondPath, firstPath, std::filesystem::copy_options::overwrite_existing);
    EXPECT_EQ(live.Reload(), ReloadOutcome::Failed);
    EXPECT_TRUE(live.IsLoaded());
    EXPECT_EQ(live.GetClusterLODManifest(), original);
    EXPECT_FLOAT_EQ(live.GetMesh(0).Vertices.front().Normal[0], 0.0f);
}

TEST_F(MeshClusterLODAssetTest, ReusesIdenticalFineStoresAndAcceptsIdenticalReloads)
{
    const auto firstPath = m_Temporary.Path() / "first.gecluster";
    const auto secondPath = m_Temporary.Path() / "second.gecluster";
    ASSERT_TRUE(Cook(firstPath)) << m_Error;
    ASSERT_TRUE(Cook(secondPath)) << m_Error;
    ModelAsset first(GUID::Generate(), firstPath);
    ModelAsset second(first.GetGUID(), secondPath);
    ASSERT_TRUE(first.Load());
    ASSERT_TRUE(second.Load());
    EXPECT_EQ(ResolveMeshClusterLODStore(firstPath, {}, *first.GetClusterLODManifest()),
              ResolveMeshClusterLODStore(secondPath, {}, *second.GetClusterLODManifest()));
    EXPECT_TRUE(first.AdoptReload(second));
}

TEST_F(MeshClusterLODAssetTest, ResolvesThePackagedStoreUnderTheAssetRoot)
{
    const auto path = m_Temporary.Path() / "author" / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    const auto editorStore = ResolveMeshClusterLODStore(path, {}, *manifest);
    const auto assetRoot = m_Temporary.Path() / "game" / "Assets";
    const auto packageStore = assetRoot / ".cluster" / editorStore.filename();
    std::filesystem::create_directories(packageStore.parent_path());
    std::filesystem::copy_file(editorStore, packageStore);
    EXPECT_EQ(ResolveMeshClusterLODStore(assetRoot / "Models" / "generated.gecluster", assetRoot, *manifest), packageStore);
}

TEST_F(MeshClusterLODAssetTest, RefusesAnimatedModelsAndKeepsThePreviousGeneratedAsset)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    Vector<uint8> before;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, before));
    m_Source.SetHasAnimationsForTest(true);
    EXPECT_FALSE(Cook(path));
    Vector<uint8> after;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, after));
    EXPECT_EQ(before, after);
}

TEST_F(MeshClusterLODAssetTest, PreservesMaterialFieldsAndOwnedImageBytes)
{
    EmbeddedImage image;
    image.MimeType = "image/x-tga";
    constexpr std::array<uint8, 22> kImageBytes{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 8, 255, 128, 64, 255};
    image.Data = ByteBuffer::CopyOf(kImageBytes);
    ImportedMaterialData material{};
    material.Name = "Opaque imported material";
    material.DiffuseTexture = "__embedded:0";
    material.NormalTexture = "__embedded:0";
    material.EmissiveTexture = GUID::Generate().ToString();
    material.DiffuseColor[0] = 0.25f;
    material.DiffuseColor[1] = 0.5f;
    material.DiffuseColor[2] = 0.75f;
    material.DiffuseColor[3] = 1.0f;
    material.SpecularColor[0] = 0.2f;
    material.EmissiveColor[2] = 0.1f;
    material.Metallic = 0.1f;
    material.Roughness = 0.8f;
    material.Shininess = 4.0f;
    material.DoubleSided = true;
    material.IgnoresVertexColor = true;
    material.DiffuseTextureTransform[0] = 2.0f;
    material.DiffuseTextureTransform[2] = 0.5f;
    material.NormalTextureTransform[5] = 3.0f;
    Vector<EmbeddedImage> images(1);
    images.front().MimeType = image.MimeType;
    images.front().Data = ByteBuffer::CopyOf(kImageBytes);
    ModelAssetFbxTestAccess::DemoteMasks(m_Source, m_Source.GetMeshes(), {material}, std::move(images));
    const auto path = m_Temporary.Path() / "textured.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    EXPECT_EQ(SerializeImportedMaterials(model.GetMaterials()), SerializeImportedMaterials(m_Source.GetMaterials()));
    ASSERT_EQ(model.GetEmbeddedImages().size(), 1u);
    EXPECT_EQ(model.GetEmbeddedImages().front().MimeType, image.MimeType);
    EXPECT_TRUE(std::ranges::equal(std::span<const uint8>(model.GetEmbeddedImages().front().Data),
                                   std::span<const uint8>(image.Data)));
    EXPECT_EQ(model.GetEmbeddedImages().front().AssetGuid, image.AssetGuid);
    ASSERT_EQ(model.GetClusterLODManifest()->TextureDependencies.size(), 1u);
    EXPECT_EQ(model.GetClusterLODManifest()->TextureDependencies.front(), GUID(material.EmissiveTexture));
}

TEST_F(MeshClusterLODAssetTest, ExtractsRegisteredTextureDependenciesThroughTheModelParser)
{
    struct CollectEdges final : DepEdgeSink
    {
        void Emit(DepEdge edge) override
        {
            std::lock_guard lock(Mutex);
            Edges.push_back(std::move(edge));
        }
        std::mutex Mutex;
        Vector<DepEdge> Edges;
    } sink;
    const GUID first = GUID::Generate();
    const GUID second = GUID::Generate();
    ImportedMaterialData material{};
    material.DiffuseTexture = first.ToString();
    material.NormalTexture = first.ToString();
    material.EmissiveTexture = second.ToString();
    ModelAssetFbxTestAccess::DemoteMasks(m_Source, m_Source.GetMeshes(), {material}, {});
    const auto path = m_Temporary.Path() / "dependencies.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    AssetMetadata metadata;
    metadata.Path = path;
    const GUID referrer = GUID::Generate();
    ModelAssetParser parser;
    ASSERT_TRUE(parser.ExtractDependencies(referrer, metadata, sink));
    ASSERT_EQ(sink.Edges.size(), 2u);
    std::set<GUID> targets;
    for (size_t i = 0; i < sink.Edges.size(); ++i)
    {
        const auto& edge = sink.Edges[i];
        EXPECT_TRUE(IsValidDepEdge(edge));
        EXPECT_EQ(edge.Referrer, referrer);
        EXPECT_EQ(edge.Kind, DepEdgeKind::MaterialTexture);
        EXPECT_EQ(edge.Ordinal, i);
        EXPECT_EQ(edge.FieldLocator, std::format("textures[{}]", i));
        targets.insert(edge.Target);
    }
    EXPECT_EQ(targets, (std::set<GUID>{first, second}));
}

TEST_F(MeshClusterLODAssetTest, RefusesNonopaqueNonfiniteAndUnresolvedTextureMaterials)
{
    const auto path = m_Temporary.Path() / "refused.gecluster";
    for (uint32 fault = 0; fault < 4; ++fault)
    {
        SCOPED_TRACE(fault);
        ImportedMaterialData material{};
        switch (fault)
        {
        case 0: material.AlphaMode = AlphaMode::Blend; break;
        case 1: material.Roughness = std::numeric_limits<float>::infinity(); break;
        case 2: material.DiffuseTexture = "missing.png"; break;
        case 3: material.NormalTexture = "__embedded:7"; break;
        }
        ModelAssetFbxTestAccess::DemoteMasks(m_Source, m_Source.GetMeshes(), {material}, {});
        EXPECT_FALSE(Cook(path));
        EXPECT_FALSE(std::filesystem::exists(path));
    }
}

// The far chain simplifies the whole coarse mesh. LOD0 stays the resident cut that streamed
// units refine, and every generated level carries the error of that cut on top of its own.
TEST_F(MeshClusterLODAssetTest, LodGenerationBuildsAFarChainOverTheUnchangedResidentCut)
{
    Mesh wavy = Testing::MakeClusterLODGrid();
    wavy.MaterialIndex = 0;
    for (auto& vertex : wavy.Vertices)
        vertex.Position[1] = std::sin(vertex.Position[0] * 0.21f) * std::sin(vertex.Position[2] * 0.19f);
    m_Source.SetMeshesForTest({std::move(wavy)});
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto original = model.GetClusterLODManifest();
    const Vector<uint32> coarseIndices = model.GetMesh(0).Indices;
    const float coarseError = MeshClusterLODRelativeCoarseError(*original, 0, model.GetMesh(0));
    ASSERT_GT(coarseError, 0.0f);
    EXPECT_GT(model.GenerateLODs({}, false), 1u);
    EXPECT_EQ(model.GetClusterLODManifest(), original);
    const Mesh& mesh = model.GetMesh(0);
    EXPECT_EQ(mesh.Indices, coarseIndices);
    ASSERT_FALSE(mesh.ExtraLODErrors.empty());
    for (size_t level = 0; level < mesh.ExtraLODErrors.size(); ++level)
        if (mesh.ExtraLODSloppy.empty() || mesh.ExtraLODSloppy[level] == 0)
            EXPECT_GE(mesh.ExtraLODErrors[level], coarseError);
    const auto cache = m_Temporary.Path() / "chain.gelod";
    EXPECT_GT(model.CookLODCache({}, false, cache), 0u);
}

TEST_F(MeshClusterLODAssetTest, RefusesRecookingAnAlreadyClusteredSource)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto recooked = m_Temporary.Path() / "coarse-repartition.gecluster";
    EXPECT_FALSE(CookMeshClusterLODAsset(model, recooked, TestConfig(), m_Error));
    EXPECT_FALSE(std::filesystem::exists(recooked));
}

TEST_F(MeshClusterLODAssetTest, RefusesAnOversizedCoarseFileBeforeReadingItsPayload)
{
    const auto path = m_Temporary.Path() / "oversized.gecluster";
    std::ofstream(path, std::ios::binary).put('x');
    std::filesystem::resize_file(path, MeshClusterLODManifest::kMaximumFileBytes + 1);
    ModelAsset model(GUID::Generate(), path);
    EXPECT_FALSE(model.Load());
    EXPECT_FALSE(model.IsLoaded());
    ModelAssetParser parser;
    AssetMetadata metadata;
    metadata.Path = path;
    EXPECT_EQ(parser.GetMaximumReadBytes(metadata), MeshClusterLODManifest::kMaximumFileBytes);
}

TEST_F(MeshClusterLODAssetTest, RejectsRehashedInvalidDirectoriesBeforePublishingDecodedData)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    Vector<uint8> valid;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, valid));
    constexpr size_t kMaterialLengthOffset = 36;
    constexpr size_t kMaterialTextOffset = 40;
    // A group record: center, radius, error, depth, terminal flag. A unit header: group, producer,
    // bounds, index count, streamed flag.
    constexpr size_t kGroupBytes = 5 * sizeof(float) + 2 * sizeof(uint32);
    uint32 materialBytes = 0;
    ASSERT_GT(valid.size(), kMaterialTextOffset);
    std::memcpy(&materialBytes, valid.data() + kMaterialLengthOffset, sizeof(materialBytes));
    const size_t meshCount = kMaterialTextOffset + materialBytes + sizeof(uint32);
    MeshClusterLODAssetData decoded;
    ASSERT_TRUE(DecodeMeshClusterLODAsset(valid, decoded, m_Error)) << m_Error;
    ASSERT_EQ(decoded.CoarseMeshes.size(), 1u);
    // The resident depth and vertex count follow the mesh placement.
    const uint32 expected[2]{decoded.Manifest.Meshes[0].ResidentDepth,
                             static_cast<uint32>(decoded.CoarseMeshes[0].Vertices.size())};
    const auto found = std::search(valid.begin() + meshCount, valid.end(), reinterpret_cast<const uint8*>(expected),
                                   reinterpret_cast<const uint8*>(expected) + sizeof(expected));
    ASSERT_NE(found, valid.end());
    const size_t residentDepth = static_cast<size_t>(found - valid.begin());
    const size_t groupCount = residentDepth + 2 * sizeof(uint32) + decoded.CoarseMeshes[0].Vertices.size() * sizeof(Vertex);
    const size_t firstGroup = groupCount + sizeof(uint32);
    const size_t groups = decoded.Manifest.Groups.size();
    const size_t unitCount = firstGroup + groups * kGroupBytes;
    const size_t firstUnit = unitCount + sizeof(uint32);
    size_t lastNonterminal = 0;
    for (size_t group = 0; group < groups; ++group)
        if (!decoded.Manifest.Groups[group].Terminal)
            lastNonterminal = group;
    for (uint32 fault = 0; fault < 14; ++fault)
    {
        SCOPED_TRACE(fault);
        Vector<uint8> bytes = valid;
        const uint32 maximumCount = std::numeric_limits<uint32>::max();
        const uint64 maximumBytes = std::numeric_limits<uint64>::max();
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        const auto write = [&bytes](size_t offset, const auto& value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
        switch (fault)
        {
        case 0: write(28, maximumBytes); break;
        case 1: write(kMaterialTextOffset + materialBytes, maximumCount); break;
        case 2: write(meshCount, maximumCount); break;
        case 3: write(residentDepth, maximumCount); break;
        case 4: write(residentDepth + sizeof(uint32), maximumCount); break;
        case 5: write(groupCount, maximumCount); break;
        case 6: write(firstGroup + 3 * sizeof(float), invalid); break;
        case 7: write(firstGroup + 5 * sizeof(float), maximumCount); break;
        case 8: write(unitCount, maximumCount); break;
        case 9: write(firstUnit, maximumCount); break;
        case 10: write(firstUnit + sizeof(uint32), static_cast<uint32>(groups)); break;
        case 11: write(firstUnit + 2 * sizeof(uint32) + 6 * sizeof(float), maximumCount); break;
        case 12: bytes[firstUnit + 3 * sizeof(uint32) + 6 * sizeof(float)] ^= 1; break;
        case 13: bytes[firstGroup + lastNonterminal * kGroupBytes + 5 * sizeof(float) + sizeof(uint32)] ^= 1; break;
        }
        constexpr size_t kHeaderBytes = 20;
        const uint64 checksum = Hashing::Fnv1a64(bytes.data() + kHeaderBytes, bytes.size() - kHeaderBytes);
        std::memcpy(bytes.data() + 12, &checksum, sizeof(checksum));
        MeshClusterLODAssetData output;
        output.Manifest.Incarnation = 77;
        EXPECT_FALSE(DecodeMeshClusterLODAsset(bytes, output, m_Error));
        EXPECT_EQ(output.Manifest.Incarnation, 77u);
    }
}

TEST_F(MeshClusterLODAssetTest, RefusesAChangedExistingStoreWithoutOverwritingEitherArtifact)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto store = ResolveMeshClusterLODStore(path, {}, *model.GetClusterLODManifest());
    std::ofstream(store, std::ios::binary | std::ios::app).put('x');
    Vector<uint8> previousModel;
    Vector<uint8> previousStore;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, previousModel));
    ASSERT_TRUE(Testing::ReadTestFileBytes(store, previousStore));
    EXPECT_FALSE(Cook(path));
    Vector<uint8> modelAfter;
    Vector<uint8> storeAfter;
    ASSERT_TRUE(Testing::ReadTestFileBytes(path, modelAfter));
    ASSERT_TRUE(Testing::ReadTestFileBytes(store, storeAfter));
    EXPECT_EQ(modelAfter, previousModel);
    EXPECT_EQ(storeAfter, previousStore);
}

TEST_F(MeshClusterLODAssetTest, EnforcesTheGroupLimitAcrossEverySourceSubmesh)
{
    const Mesh mesh = m_Source.GetMesh(0);
    MeshClusterLODDag dag;
    ASSERT_TRUE(GenerateMeshClusterLODDag(mesh, TestConfig(), dag, m_Error)) << m_Error;
    auto config = TestConfig();
    config.MaximumGroups = static_cast<uint32>(dag.Groups.size());
    const auto singlePath = m_Temporary.Path() / "single.gecluster";
    ASSERT_TRUE(CookMeshClusterLODAsset(m_Source, singlePath, config, m_Error)) << m_Error;
    m_Source.SetMeshesForTest({mesh, mesh});
    const auto doublePath = m_Temporary.Path() / "double.gecluster";
    EXPECT_FALSE(CookMeshClusterLODAsset(m_Source, doublePath, config, m_Error));
    EXPECT_FALSE(std::filesystem::exists(doublePath));
}

TEST_F(MeshClusterLODAssetTest, PackagesFinePagesOnceAndLoadsWithoutTheAuthoringTree)
{
    const auto source = m_Temporary.Path() / "author" / "generated.gecluster";
    ASSERT_TRUE(Cook(source)) << m_Error;
    const auto assetRoot = m_Temporary.Path() / "game" / "Assets";
    const auto staged = assetRoot / "Models" / "generated.gecluster";
    std::filesystem::create_directories(staged.parent_path());
    std::filesystem::copy_file(source, staged);
    ModelAssetParser parser;
    const AssetPackageCookContext context{source, staged, assetRoot};
    ASSERT_TRUE(parser.CookForPackage(context, m_Error)) << m_Error;
    ModelAsset model(GUID::Generate(), staged);
    ASSERT_TRUE(model.Load());
    const auto store = ResolveMeshClusterLODStore(staged, assetRoot, *model.GetClusterLODManifest());
    const auto modified = std::filesystem::last_write_time(store);
    ASSERT_TRUE(parser.CookForPackage(context, m_Error)) << m_Error;
    EXPECT_EQ(std::filesystem::last_write_time(store), modified);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(store.parent_path()),
                           std::filesystem::directory_iterator{}), 1);
    std::filesystem::remove_all(source.parent_path());
    Vector<uint8> bytes;
    ASSERT_TRUE(Testing::ReadTestFileBytes(store, bytes));
    EXPECT_EQ(Hashing::Fnv1a64(bytes.data(), bytes.size()), model.GetClusterLODManifest()->StoreHash);
    ModelAsset independent(GUID::Generate(), staged);
    ASSERT_TRUE(independent.Load());
    independent.PostLoad();
    EXPECT_EQ(independent.GetClusterLODManifest()->Incarnation, model.GetClusterLODManifest()->Incarnation);
}

TEST_F(MeshClusterLODAssetTest, MaterialEncodingAndDisposableCacheVersionsAreIndependent)
{
    Vector<ImportedMaterialData> materials(1);
    materials.front().Name = "Durable material";
    const String encoded = SerializeImportedMaterials(materials);
    ASSERT_TRUE(DeserializeImportedMaterials(encoded));
    const auto cache = m_Temporary.Path() / "cache.json";
    ASSERT_EQ(WriteImportedMaterialCache(cache, materials), ImportedMaterialCacheWrite::Written);
    ASSERT_TRUE(ReadImportedMaterialCache(cache));
    String cacheText;
    ASSERT_TRUE(ReadFileTextShared(cache, cacheText));
    EXPECT_NE(encoded, cacheText);
    EXPECT_FALSE(DeserializeImportedMaterials(cacheText));
    EXPECT_EQ(SerializeImportedMaterials(*ReadImportedMaterialCache(cache)), encoded);
}

TEST_F(MeshClusterLODAssetTest, OversizedInlineReloadKeepsTheExistingLoadedPayload)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto previous = model.GetClusterLODManifest();
    std::filesystem::resize_file(path, MeshClusterLODManifest::kMaximumFileBytes + 1);
    EXPECT_EQ(model.Reload(), ReloadOutcome::Failed);
    EXPECT_TRUE(model.IsLoaded());
    EXPECT_EQ(model.GetClusterLODManifest(), previous);
    EXPECT_FALSE(model.GetMesh(0).Indices.empty());
}

TEST_F(MeshClusterLODAssetTest, ManagerDrainRejectsChangedGroupsIncludingItsInlineFallback)
{
    const auto project = m_Temporary.Path() / "project";
    const auto path = project / "Assets" / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager manager;
    ASSERT_TRUE(manager.Initialize(project / "Assets", &pool, project / "AssetDatabase.assetdb", project / ".Cache"));
    // Update drains completed reloads through its enabled hot-reload path.
    manager.SetHotReloadEnabled(true);
    manager.WaitForStartupScan();
    const GUID guid = manager.ResolveAssetGuid(path);
    ASSERT_FALSE(guid.IsNull());
    auto model = std::make_shared<ModelAsset>(guid, path);
    ASSERT_TRUE(model->Load());
    manager.RegisterLoadedAsset(guid, model);
    const auto previous = model->GetClusterLODManifest();
    AssetMetadata metadata;
    ASSERT_TRUE(manager.GetRegistry().TryGetAssetMetadata(guid, metadata));
    const auto readSource = manager.GetParserRegistry().ResolveReadSource(metadata, manager);
    // The asset database can store a path in another letter case on a case-insensitive volume.
    EXPECT_TRUE(std::filesystem::equivalent(readSource.Path, path)) << readSource.Path;
    EXPECT_EQ(readSource.MaximumBytes, MeshClusterLODManifest::kMaximumFileBytes);
    Mesh changed = m_Source.GetMesh(0);
    for (Vertex& vertex : changed.Vertices)
        vertex.Normal[0] = 0.1f;
    m_Source.SetMeshesForTest({std::move(changed)});
    ASSERT_TRUE(Cook(path)) << m_Error;
    ASSERT_TRUE(manager.RequestAsyncReload(guid));
    ASSERT_TRUE(manager.IsReloadInFlight(guid));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (manager.IsReloadInFlight(guid) && std::chrono::steady_clock::now() < deadline)
    {
        manager.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(manager.IsReloadInFlight(guid));
    EXPECT_EQ(manager.GetAsset(guid).get(), model.get());
    EXPECT_TRUE(model->IsLoaded());
    EXPECT_EQ(model->GetClusterLODManifest(), previous);
    EXPECT_FLOAT_EQ(model->GetMesh(0).Vertices.front().Normal[0], 0.0f);
    manager.Shutdown();
    pool.Shutdown();
}

} // namespace
} // namespace GameEngine
