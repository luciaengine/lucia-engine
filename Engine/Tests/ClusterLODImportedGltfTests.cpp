#include "Assets/ClusterLOD/MeshClusterLODAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/MeshLODGenerator.h"
#include "ClusterLODTestMesh.h"
#include "GltfTestFiles.h"
#include "Mathematics/Matrix4x4.h"
#include "TestTempDir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
enum class GltfMotion : uint8 { Static, Animated, Skinned };

Vector<uint8> ImportedTriangle(GltfMotion motion)
{
    auto document = nlohmann::json::parse(R"({
        "asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"translation":[3,2,5],"scale":[2,3,4],"children":[1,2]},
                 {"mesh":0,"translation":[7,0,1]},{"mesh":0,"translation":[-4,0,2]}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
                       {"buffer":0,"byteOffset":36,"byteLength":12}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},
                     {"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]
    })");
    String binary;
    const std::array<Mathematics::Vector3, 3> positions{{{1,0,0}, {0,1,0}, {0,0,1}}};
    for (const auto& position : positions)
        for (const float coordinate : {position.x, position.y, position.z})
            TestFiles::AppendUint32(binary, std::bit_cast<uint32>(coordinate));
    for (const uint32 index : {0u, 1u, 2u})
        TestFiles::AppendUint32(binary, index);
    if (motion == GltfMotion::Animated)
    {
        for (const float time : {0.0f, 1.0f})
            TestFiles::AppendUint32(binary, std::bit_cast<uint32>(time));
        for (const float coordinate : {7.0f, 0.0f, 1.0f, 8.0f, 0.0f, 1.0f})
            TestFiles::AppendUint32(binary, std::bit_cast<uint32>(coordinate));
        document["bufferViews"].push_back({{"buffer",0},{"byteOffset",48},{"byteLength",8}});
        document["bufferViews"].push_back({{"buffer",0},{"byteOffset",56},{"byteLength",24}});
        document["accessors"].push_back({{"bufferView",2},{"componentType",5126},{"count",2},{"type","SCALAR"},{"min",{0}},{"max",{1}}});
        document["accessors"].push_back({{"bufferView",3},{"componentType",5126},{"count",2},{"type","VEC3"}});
        document["animations"] = {{{"samplers",{{{"input",2},{"output",3},{"interpolation","LINEAR"}}}},
                                    {"channels",{{{"sampler",0},{"target",{{"node",1},{"path","translation"}}}}}}}};
    }
    else if (motion == GltfMotion::Skinned)
    {
        binary.append(24, '\0'); // Three JOINTS_0 vectors of four uint16 zero joint indices.
        for (uint32 vertex = 0; vertex < 3; ++vertex)
            for (const float weight : {1.0f, 0.0f, 0.0f, 0.0f})
                TestFiles::AppendUint32(binary, std::bit_cast<uint32>(weight));
        const auto inverseBind = Mathematics::Matrix4x4::Identity();
        for (uint32 element = 0; element < 16; ++element)
            TestFiles::AppendUint32(binary, std::bit_cast<uint32>(inverseBind.Data()[element]));
        document["bufferViews"].push_back({{"buffer",0},{"byteOffset",48},{"byteLength",24}});
        document["bufferViews"].push_back({{"buffer",0},{"byteOffset",72},{"byteLength",48}});
        document["bufferViews"].push_back({{"buffer",0},{"byteOffset",120},{"byteLength",64}});
        document["accessors"].push_back({{"bufferView",2},{"componentType",5123},{"count",3},{"type","VEC4"}});
        document["accessors"].push_back({{"bufferView",3},{"componentType",5126},{"count",3},{"type","VEC4"}});
        document["accessors"].push_back({{"bufferView",4},{"componentType",5126},{"count",1},{"type","MAT4"}});
        document["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"] = 2;
        document["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_0"] = 3;
        document["skins"] = {{{"joints",{0}},{"skeleton",0},{"inverseBindMatrices",4}}};
        document["nodes"][1]["skin"] = 0;
        document["nodes"][2]["skin"] = 0;
    }
    document["buffers"] = {{{"byteLength",binary.size()}}};
    const auto bytes = TestFiles::BinaryGltf(document.dump(), binary);
    return {bytes.begin(), bytes.end()};
}

void AppendPlacedVertices(const Mesh& mesh, const float transform[16], Vector<Mathematics::Vector3>& output)
{
    const auto matrix = Mathematics::Matrix4x4::FromColumnMajor(transform);
    for (const auto& vertex : mesh.Vertices)
        output.push_back(matrix.TransformPoint({vertex.Position[0], vertex.Position[1], vertex.Position[2]}));
}

void ExpectPlacedTriangles(const Mesh& mesh)
{
    ASSERT_EQ(mesh.Vertices.size(), 3u);
    ASSERT_EQ(mesh.Indices.size(), 3u);
    for (const uint32 index : mesh.Indices)
        ASSERT_LT(index, mesh.Vertices.size());
    EXPECT_NE(mesh.Indices[0], mesh.Indices[1]);
    EXPECT_NE(mesh.Indices[0], mesh.Indices[2]);
    EXPECT_NE(mesh.Indices[1], mesh.Indices[2]);
    Vector<Mathematics::Vector3> actual;
    AppendPlacedVertices(mesh, mesh.SourceNodeTransform, actual);
    ASSERT_EQ(mesh.ExtraPlacements.size(), 1u);
    AppendPlacedVertices(mesh, mesh.ExtraPlacements.front().SourceNodeTransform, actual);
    // Default glTF import mirrors X. Parent scale applies to each child translation and vertex.
    const std::array<Mathematics::Vector3, 6> expected{{{-19,2,9}, {-17,5,9}, {-17,2,13},
                                                     {3,2,13}, {5,5,13}, {5,2,17}}};
    ASSERT_EQ(actual.size(), expected.size());
    constexpr float kPositionTolerance = 1.0e-5f;
    for (const auto& point : expected)
        EXPECT_TRUE(std::any_of(actual.begin(), actual.end(), [point](const auto& position)
        {
            return std::abs(position.x-point.x) < kPositionTolerance &&
                   std::abs(position.y-point.y) < kPositionTolerance &&
                   std::abs(position.z-point.z) < kPositionTolerance;
        })) << point.x << ',' << point.y << ',' << point.z;
}

class ClusterLODImportedGltfTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!IsMeshLODGenerationAvailable())
            GTEST_SKIP() << "meshoptimizer not compiled in";
    }

    void ExpectRefusedWithoutReplacingOutput(const ModelAsset& source)
    {
        ModelAsset previous(GUID::Generate(), m_Temporary.Path()/"previous.obj");
        auto mesh = Testing::MakeClusterLODGrid();
        mesh.MaterialIndex = 0;
        previous.SetMeshesForTest({std::move(mesh)});
        const auto path = m_Temporary.Path()/"previous.gecluster";
        String error;
        ASSERT_TRUE(CookMeshClusterLODAsset(previous, path, {}, error)) << error;
        Vector<uint8> before;
        ASSERT_TRUE(ReadFileBytesShared(path, before));
        EXPECT_FALSE(CookMeshClusterLODAsset(source, path, {}, error));
        Vector<uint8> after;
        ASSERT_TRUE(ReadFileBytesShared(path, after));
        EXPECT_EQ(before, after);
    }

    TestUtils::ScopedTempDir m_Temporary{TestUtils::MakeUniqueTempDirectory("cluster_gltf")};
};

TEST_F(ClusterLODImportedGltfTest, StaticHierarchyCooksBothPlacementsIntoCoarseAndFineGeometry)
{
    ModelAsset source(GUID::Generate(), m_Temporary.Path()/"static.glb");
    ASSERT_TRUE(source.LoadFromData(ImportedTriangle(GltfMotion::Static)));
    ASSERT_TRUE(source.IsLoaded());
    ASSERT_NE(source.GetSkeletonId(), 0u);
    ASSERT_FALSE(source.HasAnimations());
    ASSERT_EQ(source.GetClusterLODManifest(), nullptr);
    ASSERT_LE(source.GetMaterials().size(), 1u);
    ASSERT_EQ(source.GetMeshCount(), 1u);
    ASSERT_FALSE(source.GetMesh(0).Skinned);
    ASSERT_FALSE(source.GetMesh(0).HasMorphTargets());
    ASSERT_TRUE(source.GetMesh(0).Joints0.empty());
    ASSERT_TRUE(source.GetMesh(0).Weights0.empty());
    ExpectPlacedTriangles(source.GetMesh(0));
    SCOPED_TRACE(::testing::Message() << "static hierarchy skeleton=" << source.GetSkeletonId());
    String error;
    const auto path = m_Temporary.Path()/"static.gecluster";
    ASSERT_EQ(ModelAsset::FormatFromPath(path), ModelFormat::ClusterLOD);
    ASSERT_TRUE(CookMeshClusterLODAsset(source, path, {}, error)) << error;
    ModelAsset cooked(GUID::Generate(), path);
    ASSERT_TRUE(cooked.Load());
    EXPECT_EQ(cooked.GetSkeletonId(), 0u);
    ASSERT_EQ(cooked.GetMeshCount(), 1u);
    ExpectPlacedTriangles(cooked.GetMesh(0));
    const auto manifest = cooked.GetClusterLODManifest();
    ASSERT_NE(manifest, nullptr);
    ASSERT_EQ(manifest->Pages.size(), 1u);
    const auto& page = manifest->Pages.front();
    SharedFileReader store(ResolveMeshClusterLODStore(path, {}, *manifest));
    Vector<uint8> bytes(static_cast<size_t>(page.Bytes));
    ASSERT_TRUE(store.SeekTo(page.Offset));
    ASSERT_EQ(store.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    Mesh fine;
    ASSERT_TRUE(DecodeMeshClusterLODFinePage(bytes, page, cooked.GetMesh(0), fine, error)) << error;
    ExpectPlacedTriangles(fine);
}

TEST_F(ClusterLODImportedGltfTest, RigidAnimationIsRefusedWithoutReplacingThePublishedAsset)
{
    ModelAsset source(GUID::Generate(), m_Temporary.Path()/"animated.glb");
    ASSERT_TRUE(source.LoadFromData(ImportedTriangle(GltfMotion::Animated)));
    ASSERT_TRUE(source.HasAnimations());
    ASSERT_EQ(source.GetMeshCount(), 1u);
    ASSERT_FALSE(source.GetMesh(0).Skinned);
    ExpectRefusedWithoutReplacingOutput(source);
}

TEST_F(ClusterLODImportedGltfTest, UnanimatedSkinIsRefusedWithoutReplacingThePublishedAsset)
{
    ModelAsset source(GUID::Generate(), m_Temporary.Path()/"skinned.glb");
    ASSERT_TRUE(source.LoadFromData(ImportedTriangle(GltfMotion::Skinned)));
    ASSERT_FALSE(source.HasAnimations());
    ASSERT_EQ(source.GetMeshCount(), 1u);
    ASSERT_TRUE(source.GetMesh(0).IsSkinned());
    ExpectRefusedWithoutReplacingOutput(source);
}
} // namespace
} // namespace GameEngine
