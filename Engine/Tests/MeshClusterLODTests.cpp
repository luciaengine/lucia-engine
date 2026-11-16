#include "Assets/ClusterLOD/MeshClusterLODGenerator.h"

#include "AssetCore/SharedFileRead.h"
#include "AssetCore/DepEdge.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/Parsers/ModelAssetParser.h"
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
    Vector<MeshClusterLODGroup> groups(1);
    groups.front().Coarse.Name = "Unpublished";
    String error;
    EXPECT_FALSE(GenerateMeshClusterLODs(mesh, config, groups, error));
    EXPECT_FALSE(error.empty());
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups.front().Coarse.Name, "Unpublished");
}

// Counts the edges of a cut that leave a crack: an edge that only one triangle uses and that is
// not open in the source, an edge that more than two triangles use, or a shared edge that both
// triangles walk in the same direction.
size_t CountCutCracks(const Vector<const Mesh*>& cut, const std::set<Edge>& sourceBoundary)
{
    std::map<Edge, uint32> counts;
    std::map<Edge, int32> orientation;
    for (const Mesh* mesh : cut)
    {
        for (const Triangle& triangle : OrientedTriangles(*mesh))
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
    }
    size_t cracks = 0;
    for (const auto& [edge, count] : counts)
    {
        const bool open = count == 1 && !sourceBoundary.contains(edge);
        cracks += open || count > 2 || (count == 2 && orientation[edge] != 0) ? 1 : 0;
    }
    return cracks;
}

// The cracks of the cut that draws the fine page of each refined group and the coarse mesh of
// every other group of a cooked model.
size_t CountSelectedCutCracks(const ModelAsset& model, const Vector<Mesh>& fine, const Vector<bool>& refined,
                              const std::set<Edge>& sourceBoundary)
{
    Vector<const Mesh*> cut;
    cut.reserve(refined.size());
    for (uint32 group = 0; group < refined.size(); ++group)
        cut.push_back(refined[group] ? &fine[group] : &model.GetMesh(group));
    return CountCutCracks(cut, sourceBoundary);
}

bool IsMixedCut(const Vector<bool>& refined)
{
    const auto fineGroups = std::count(refined.begin(), refined.end(), true);
    return fineGroups != 0 && fineGroups != static_cast<std::ptrdiff_t>(refined.size());
}

TEST(MeshClusterLOD, PartitionsEveryOrientedSourceTriangleExactlyOnce)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), groups, error)) << error;
    ASSERT_GT(groups.size(), 1u);
    Vector<Triangle> triangles;
    size_t coarseIndices = 0;
    for (const MeshClusterLODGroup& group : groups)
    {
        ASSERT_FALSE(group.Fine.Indices.empty());
        ASSERT_FALSE(group.Coarse.Indices.empty());
        const auto fineTriangles = OrientedTriangles(group.Fine);
        triangles.insert(triangles.end(), fineTriangles.begin(), fineTriangles.end());
        coarseIndices += group.Coarse.Indices.size();
        EXPECT_EQ(group.Coarse.MaterialIndex, source.MaterialIndex);
        EXPECT_EQ(group.Fine.Name, source.Name);
        EXPECT_GE(group.Error, 0.0f);
    }
    std::sort(triangles.begin(), triangles.end());
    EXPECT_EQ(triangles, OrientedTriangles(source));
    EXPECT_LT(coarseIndices, source.Indices.size());
    EXPECT_EQ(source.Vertices.size(), kGridWidth * kGridWidth);
    EXPECT_EQ(source.Indices.size(), (kGridWidth - 1) * (kGridWidth - 1) * 6u);
}

TEST(MeshClusterLOD, EmitsOnlyPositiveAreaCoarseTrianglesOnThePlanarGrid)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(Testing::MakeClusterLODGrid(), TestConfig(), groups, error)) << error;
    for (size_t group = 0; group < groups.size(); ++group)
    {
        const auto& mesh = groups[group].Coarse;
        for (size_t offset = 0; offset < mesh.Indices.size(); offset += 3)
        {
            SCOPED_TRACE(std::format("group {} triangle {}", group, offset / 3));
            const auto& first = mesh.Vertices.at(mesh.Indices[offset]).Position;
            const auto& second = mesh.Vertices.at(mesh.Indices[offset + 1]).Position;
            const auto& third = mesh.Vertices.at(mesh.Indices[offset + 2]).Position;
            const float ax = second[0] - first[0];
            const float ay = second[1] - first[1];
            const float az = second[2] - first[2];
            const float bx = third[0] - first[0];
            const float by = third[1] - first[1];
            const float bz = third[2] - first[2];
            const float x = ay * bz - az * by;
            const float y = az * bx - ax * bz;
            const float z = ax * by - ay * bx;
            EXPECT_GT(x * x + y * y + z * z, 0.0f);
        }
    }
}

TEST(MeshClusterLOD, PreservesEveryGroupBoundaryEdgeAndFineCullingEnvelope)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(Testing::MakeClusterLODGrid(), TestConfig(), groups, error)) << error;
    std::map<Edge, uint32> groupEdges;
    for (const MeshClusterLODGroup& group : groups)
    {
        const auto boundary = BoundaryEdges(group.Fine);
        EXPECT_EQ(BoundaryEdges(group.Coarse), boundary);
        for (const Edge& edge : boundary)
            ++groupEdges[edge];
        for (size_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_EQ(group.Coarse.MinBounds[axis], group.Fine.MinBounds[axis]);
            EXPECT_EQ(group.Coarse.MaxBounds[axis], group.Fine.MaxBounds[axis]);
        }
    }
    EXPECT_GT(std::count_if(groupEdges.begin(), groupEdges.end(),
        [](const auto& entry) { return entry.second == 2; }), 0);
}

TEST(MeshClusterLOD, RepeatsTheSamePartitionAndSimplification)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    Vector<MeshClusterLODGroup> first;
    Vector<MeshClusterLODGroup> second;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), first, error)) << error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), second, error)) << error;
    ASSERT_EQ(first.size(), second.size());
    for (size_t group = 0; group < first.size(); ++group)
    {
        EXPECT_EQ(first[group].Fine.Indices, second[group].Fine.Indices);
        EXPECT_EQ(first[group].Coarse.Indices, second[group].Coarse.Indices);
        ASSERT_EQ(first[group].Coarse.Vertices.size(), second[group].Coarse.Vertices.size());
        EXPECT_EQ(std::memcmp(first[group].Coarse.Vertices.data(), second[group].Coarse.Vertices.data(),
            first[group].Coarse.Vertices.size() * sizeof(Vertex)), 0);
        EXPECT_EQ(first[group].Error, second[group].Error);
    }
}

TEST(MeshClusterLOD, ConvertsNonzeroSimplificationErrorToLocalUnits)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    for (Vertex& vertex : source.Vertices)
        vertex.Position[1] = 4.0f * std::sin(vertex.Position[0] * 0.2f) * std::sin(vertex.Position[2] * 0.15f);
    auto config = TestConfig();
    config.MeshletsPerGroup = 128;
    Vector<MeshClusterLODGroup> original;
    Vector<MeshClusterLODGroup> scaled;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, config, original, error)) << error;
    ASSERT_EQ(original.size(), 1u);
    ASSERT_GT(original.front().Error, 0.0f);
    ASSERT_LT(original.front().Coarse.Indices.size(), source.Indices.size());
    constexpr float kScale = 4.0f;
    for (Vertex& vertex : source.Vertices)
        for (float& component : vertex.Position)
            component *= kScale;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, config, scaled, error)) << error;
    ASSERT_EQ(scaled.size(), 1u);
    EXPECT_EQ(original.front().Coarse.Indices, scaled.front().Coarse.Indices);
    EXPECT_NEAR(scaled.front().Error, original.front().Error * kScale, original.front().Error * 0.001f);
}

TEST(MeshClusterLOD, MixedCurvedCutsKeepOppositeDirectedSharedEdges)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    for (Vertex& vertex : source.Vertices)
        vertex.Position[1] = 2.0f * std::sin(vertex.Position[0] * 0.2f) * std::cos(vertex.Position[2] * 0.2f);
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), groups, error)) << error;
    ASSERT_GT(groups.size(), 1u);
    std::map<Edge, int32> orientation;
    std::map<Edge, uint32> counts;
    size_t reducedGroups = 0;
    for (size_t group = 0; group < groups.size(); ++group)
    {
        const Mesh& mesh = group % 2 == 0 ? groups[group].Coarse : groups[group].Fine;
        for (const Vertex& vertex : mesh.Vertices)
            EXPECT_TRUE(std::any_of(source.Vertices.begin(), source.Vertices.end(),
                [&vertex](const Vertex& original) { return std::memcmp(&vertex, &original, sizeof(Vertex)) == 0; }));
        if (mesh.Indices.size() < groups[group].Fine.Indices.size())
            ++reducedGroups;
        for (const Triangle& triangle : OrientedTriangles(mesh))
        {
            for (size_t corner = 0; corner < 3; ++corner)
            {
                const uint32 first = triangle[corner];
                const uint32 second = triangle[(corner + 1) % 3];
                const Edge edge{std::min(first, second), std::max(first, second)};
                orientation[edge] += first < second ? 1 : -1;
                ++counts[edge];
            }
        }
    }
    EXPECT_GT(reducedGroups, 0u);
    const auto sourceBoundary = BoundaryEdges(source);
    for (const auto& [edge, count] : counts)
    {
        if (sourceBoundary.contains(edge))
            EXPECT_EQ(count, 1u);
        else
        {
            EXPECT_EQ(count, 2u) << edge.first << ',' << edge.second;
            EXPECT_EQ(orientation[edge], 0) << edge.first << ',' << edge.second;
        }
    }
}

TEST(MeshClusterLOD, PreservesDuplicatePositionWedgeAttributes)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    constexpr uint32 kSeamColumn = kGridWidth / 2;
    for (uint32 row = 0; row < kGridWidth; ++row)
    {
        Vertex duplicate = source.Vertices[row * kGridWidth + kSeamColumn];
        duplicate.TexCoords[0] += 1.0f;
        duplicate.Normal[0] = 0.25f;
        duplicate.Tangent[3] = -1.0f;
        source.Vertices.push_back(duplicate);
    }
    for (size_t offset = 0; offset < source.Indices.size(); offset += 3)
    {
        const bool rightSide = std::any_of(source.Indices.begin() + offset,
            source.Indices.begin() + offset + 3, [](uint32 index)
            { return index % kGridWidth > kSeamColumn; });
        if (!rightSide)
            continue;
        for (size_t corner = offset; corner < offset + 3; ++corner)
            if (source.Indices[corner] % kGridWidth == kSeamColumn)
                source.Indices[corner] = kGridWidth * kGridWidth + source.Indices[corner] / kGridWidth;
    }
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), groups, error)) << error;
    std::set<std::pair<uint32, bool>> seamWedges;
    for (const MeshClusterLODGroup& group : groups)
    {
        for (const Vertex& vertex : group.Coarse.Vertices)
        {
            EXPECT_TRUE(std::any_of(source.Vertices.begin(), source.Vertices.end(),
                [&vertex](const Vertex& original) { return std::memcmp(&vertex, &original, sizeof(Vertex)) == 0; }));
            if (vertex.Position[0] == kSeamColumn)
                seamWedges.emplace(static_cast<uint32>(vertex.Position[2]), vertex.Normal[0] != 0.0f);
        }
    }
    EXPECT_EQ(seamWedges.size(), kGridWidth * 2u);
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

TEST(MeshClusterLOD, RejectsNonManifoldAndInconsistentWindingWithoutPublishing)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    const Triangle duplicate{source.Indices[0], source.Indices[1], source.Indices[2]};
    source.Indices.insert(source.Indices.end(), duplicate.begin(), duplicate.end());
    ExpectRefusal(source, TestConfig());
    source = Testing::MakeClusterLODGrid();
    std::swap(source.Indices[0], source.Indices[1]);
    ExpectRefusal(source, TestConfig());
    source = Testing::MakeClusterLODGrid();
    source.Indices = {0, kGridWidth, 1, 0, kGridWidth + 1, 2};
    ExpectRefusal(source, TestConfig());
    source.Indices = {0, kGridWidth, 1, 1, kGridWidth, 0};
    ExpectRefusal(source, TestConfig());
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

TEST(MeshClusterLOD, PreservesTheSourcePlacementAndEveryRepeatedPlacement)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh source = Testing::MakeClusterLODGrid();
    source.SourceNodeIndex = 13;
    source.SourceNodeParentIndex = 2;
    source.SourceNodeTransform[12] = 9.0f;
    source.SourceNodeLocalTransform[13] = 3.0f;
    MeshPlacement placement;
    placement.SourceNodeIndex = 21;
    placement.SourceNodeTransform[14] = 11.0f;
    placement.SourceNodeLocalTransform[0] = 2.0f;
    source.ExtraPlacements.push_back(placement);
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, TestConfig(), groups, error)) << error;
    for (const MeshClusterLODGroup& group : groups)
    {
        for (const Mesh* mesh : {&group.Coarse, &group.Fine})
        {
            EXPECT_EQ(mesh->SourceNodeIndex, source.SourceNodeIndex);
            EXPECT_EQ(mesh->SourceNodeParentIndex, source.SourceNodeParentIndex);
            EXPECT_TRUE(std::equal(std::begin(mesh->SourceNodeTransform), std::end(mesh->SourceNodeTransform),
                                  std::begin(source.SourceNodeTransform)));
            EXPECT_TRUE(std::equal(std::begin(mesh->SourceNodeLocalTransform), std::end(mesh->SourceNodeLocalTransform),
                                  std::begin(source.SourceNodeLocalTransform)));
            ASSERT_EQ(mesh->ExtraPlacements.size(), 1u);
            const MeshPlacement& actual = mesh->ExtraPlacements.front();
            EXPECT_EQ(actual.SourceNodeIndex, placement.SourceNodeIndex);
            EXPECT_TRUE(std::equal(std::begin(actual.SourceNodeTransform), std::end(actual.SourceNodeTransform),
                                  std::begin(placement.SourceNodeTransform)));
            EXPECT_TRUE(std::equal(std::begin(actual.SourceNodeLocalTransform), std::end(actual.SourceNodeLocalTransform),
                                  std::begin(placement.SourceNodeLocalTransform)));
        }
    }
}

TEST(MeshClusterLOD, RefusesOutputThatExceedsAnAdmittedSourcePayloadLimit)
{
    if (!IsMeshLODGenerationAvailable())
        GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh source = Testing::MakeClusterLODGrid();
    auto config = TestConfig();
    Vector<MeshClusterLODGroup> groups;
    String error;
    ASSERT_TRUE(GenerateMeshClusterLODs(source, config, groups, error)) << error;
    uint64 outputBytes = 0;
    for (const MeshClusterLODGroup& group : groups)
        outputBytes += (group.Coarse.Vertices.size() + group.Fine.Vertices.size()) * sizeof(Vertex) +
            (group.Coarse.Indices.size() + group.Fine.Indices.size()) * sizeof(uint32);
    const uint64 sourceBytes = source.Vertices.size() * sizeof(Vertex) + source.Indices.size() * sizeof(uint32);
    ASSERT_GT(outputBytes, sourceBytes);
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

TEST_F(MeshClusterLODAssetTest, LoadsTheCoarseAssetWithoutTheSourceOrFineStore)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    ASSERT_GT(model.GetMeshCount(), 1u);
    EXPECT_EQ(manifest->Pages.size(), model.GetMeshCount());
    ASSERT_TRUE(std::filesystem::remove(ResolveMeshClusterLODStore(path, {}, *manifest)));
    EXPECT_FALSE(std::filesystem::exists(m_Source.GetPath()));
    ModelAsset coarseOnly(GUID::Generate(), path);
    ASSERT_TRUE(coarseOnly.Load());
    coarseOnly.PostLoad();
    EXPECT_EQ(coarseOnly.GetMeshCount(), model.GetMeshCount());
    std::set<String> names;
    for (const Mesh& mesh : coarseOnly.GetMeshes())
    {
        EXPECT_TRUE(mesh.ExtraLODs.empty());
        EXPECT_FALSE(mesh.Indices.empty());
        EXPECT_TRUE(names.insert(mesh.Name).second);
    }
}

TEST_F(MeshClusterLODAssetTest, FineRangesReconstructEveryOrientedSourceTriangle)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    ASSERT_EQ(store.Size(), static_cast<int64>(manifest->StoreBytes));
    Vector<Triangle> triangles;
    for (uint32 group = 0; group < model.GetMeshCount(); ++group)
    {
        const auto& page = manifest->Pages[group];
        Vector<uint8> bytes(static_cast<size_t>(page.Bytes));
        ASSERT_TRUE(store.SeekTo(page.Offset));
        ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
        Mesh fine;
        ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, page, model.GetMesh(group), fine, m_Error)) << m_Error;
        const auto groupTriangles = OrientedTriangles(fine);
        triangles.insert(triangles.end(), groupTriangles.begin(), groupTriangles.end());
        EXPECT_EQ(BoundaryEdges(fine), BoundaryEdges(model.GetMesh(group)));
    }
    std::sort(triangles.begin(), triangles.end());
    EXPECT_EQ(triangles, OrientedTriangles(m_Source.GetMesh(0)));
}

// Selects cuts of the cooked model with the runtime rule (ClusterGroupErrorPx against the pixel
// budget) from several cameras and budgets, and every largest-error-first prefix that a triangle
// budget can select. No cut may open an edge that is closed in the source.
TEST_F(MeshClusterLODAssetTest, EverySelectedCutOfTheCookedModelIsCrackFree)
{
    Mesh curved = Testing::MakeClusterLODGrid();
    curved.MaterialIndex = 0;
    for (Vertex& vertex : curved.Vertices)
        vertex.Position[1] = 2.0f * std::sin(vertex.Position[0] * 0.2f) * std::cos(vertex.Position[2] * 0.2f);
    m_Source.SetMeshesForTest({curved});
    const auto path = m_Temporary.Path() / "cracks.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto manifest = model.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    const uint32 groupCount = model.GetMeshCount();
    Vector<Mesh> fine(groupCount);
    size_t reducedGroups = 0;
    for (uint32 group = 0; group < groupCount; ++group)
    {
        const auto& page = manifest->Pages[group];
        Vector<uint8> bytes(static_cast<size_t>(page.Bytes));
        ASSERT_TRUE(store.SeekTo(page.Offset));
        ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
        ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, page, model.GetMesh(group), fine[group], m_Error)) << m_Error;
        reducedGroups += page.IndexCount > model.GetMesh(group).Indices.size() && page.Error > 0.0f ? 1 : 0;
    }
    ASSERT_GT(reducedGroups, groupCount / 2) << "the cuts must choose between real reductions";
    const auto sourceBoundary = BoundaryEdges(curved);

    // The feature bounds the instance scale by the Frobenius norm of its basis: sqrt(3) for identity.
    const double scale = std::sqrt(3.0);
    constexpr double kVerticalFieldOfView = 1.0471976; // 60 degrees
    const double projectionScaleY = 1.0 / std::tan(0.5 * kVerticalFieldOfView);
    constexpr uint32 kViewportHeight = 1080;
    const std::array<Mathematics::Vector3, 5> eyes{{
        {24.0f, 3.0f, 24.0f}, {-6.0f, 4.0f, -6.0f}, {60.0f, 10.0f, 24.0f}, {10.0f, 1.5f, 40.0f}, {24.0f, 80.0f, -40.0f}}};
    const std::array<float, 4> budgetsPx{0.25f, 1.0f, 4.0f, 16.0f};
    size_t mixedCuts = 0;
    size_t cuts = 0;
    Vector<double> errorPx(groupCount);
    Vector<uint32> order(groupCount);
    for (const auto& eye : eyes)
    {
        for (uint32 group = 0; group < groupCount; ++group)
        {
            const Mesh& coarse = model.GetMesh(group);
            const Mathematics::Vector3 low(coarse.MinBounds[0], coarse.MinBounds[1], coarse.MinBounds[2]);
            const Mathematics::Vector3 high(coarse.MaxBounds[0], coarse.MaxBounds[1], coarse.MaxBounds[2]);
            const double radius = (high - low).Length() * 0.5 * scale;
            const double centerDistance = ((low + high) * 0.5f - eye).Length();
            errorPx[group] = Rendering::ClusterGroupErrorPx(manifest->Pages[group].Error, scale, centerDistance, radius,
                                                            projectionScaleY, kViewportHeight, 0.0f, false);
        }
        for (const float budgetPx : budgetsPx)
        {
            Vector<bool> refined(groupCount);
            for (uint32 group = 0; group < groupCount; ++group)
                refined[group] = errorPx[group] > budgetPx * Rendering::kClusterRefineBudgetScale;
            EXPECT_EQ(CountSelectedCutCracks(model, fine, refined, sourceBoundary), 0u)
                << "eye " << eye.x << ',' << eye.y << ',' << eye.z << " budget " << budgetPx;
            mixedCuts += IsMixedCut(refined) ? 1 : 0;
            ++cuts;
        }
        for (uint32 group = 0; group < groupCount; ++group)
            order[group] = group;
        std::stable_sort(order.begin(), order.end(), [&errorPx](uint32 a, uint32 b) { return errorPx[a] > errorPx[b]; });
        Vector<bool> refined(groupCount, false);
        for (uint32 prefix = 0; prefix < groupCount; ++prefix)
        {
            refined[order[prefix]] = true;
            EXPECT_EQ(CountSelectedCutCracks(model, fine, refined, sourceBoundary), 0u)
                << "eye " << eye.x << ',' << eye.y << ',' << eye.z << " prefix " << prefix + 1;
            mixedCuts += IsMixedCut(refined) ? 1 : 0;
            ++cuts;
        }
    }
    EXPECT_GT(mixedCuts, cuts / 2) << "most cuts must mix coarse and fine groups";
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
    const auto& page = manifest->Pages.front();
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    Vector<uint8> bytes(static_cast<size_t>(page.Bytes));
    ASSERT_TRUE(store.SeekTo(page.Offset));
    ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    bytes.back() ^= 0x80;
    Mesh output;
    output.Name = "Previous valid mesh";
    EXPECT_FALSE(DecodeMeshClusterLODFinePage(bytes, page, model.GetMesh(0), output, m_Error));
    EXPECT_EQ(output.Name, "Previous valid mesh");
    bytes.back() ^= 0x80;
    ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, page, model.GetMesh(0), output, m_Error)) << m_Error;
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

TEST_F(MeshClusterLODAssetTest, ManualLodGenerationAndCookingKeepTheSingleCookedLevel)
{
    const auto path = m_Temporary.Path() / "generated.gecluster";
    ASSERT_TRUE(Cook(path)) << m_Error;
    ModelAsset model(GUID::Generate(), path);
    ASSERT_TRUE(model.Load());
    const auto original = model.GetClusterLODManifest();
    Vector<std::set<Edge>> boundaries;
    for (const Mesh& mesh : model.GetMeshes())
        boundaries.push_back(BoundaryEdges(mesh));
    EXPECT_EQ(model.GenerateLODs({}, false), 1u);
    const auto cache = m_Temporary.Path() / "refused.gelod";
    EXPECT_EQ(model.CookLODCache({}, false, cache), 0u);
    EXPECT_FALSE(std::filesystem::exists(cache));
    EXPECT_EQ(model.GetClusterLODManifest(), original);
    for (size_t group = 0; group < boundaries.size(); ++group)
    {
        EXPECT_TRUE(model.GetMesh(static_cast<uint32>(group)).ExtraLODs.empty());
        EXPECT_EQ(BoundaryEdges(model.GetMesh(static_cast<uint32>(group))), boundaries[group]);
    }
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
    uint32 materialBytes = 0;
    ASSERT_GT(valid.size(), kMaterialTextOffset);
    std::memcpy(&materialBytes, valid.data() + kMaterialLengthOffset, sizeof(materialBytes));
    const size_t firstPage = kMaterialTextOffset + materialBytes + 2 * sizeof(uint32);
    ASSERT_GT(valid.size(), firstPage + 36);
    for (uint32 fault = 0; fault < 5; ++fault)
    {
        SCOPED_TRACE(fault);
        Vector<uint8> bytes = valid;
        const uint32 maximumCount = std::numeric_limits<uint32>::max();
        const uint64 maximumBytes = std::numeric_limits<uint64>::max();
        switch (fault)
        {
        case 0: std::memcpy(bytes.data() + 28, &maximumBytes, sizeof(maximumBytes)); break;
        case 1: std::memcpy(bytes.data() + kMaterialTextOffset + materialBytes, &maximumCount, sizeof(maximumCount)); break;
        case 2: std::memcpy(bytes.data() + firstPage - sizeof(uint32), &maximumCount, sizeof(maximumCount)); break;
        case 3: std::memcpy(bytes.data() + firstPage + 24, &maximumCount, sizeof(maximumCount)); break;
        case 4:
        {
            const float invalid = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(bytes.data() + firstPage + 32, &invalid, sizeof(invalid));
            break;
        }
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
    Vector<MeshClusterLODGroup> generated;
    ASSERT_TRUE(GenerateMeshClusterLODs(mesh, TestConfig(), generated, m_Error)) << m_Error;
    auto config = TestConfig();
    config.MaximumGroups = static_cast<uint32>(generated.size());
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
