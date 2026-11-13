// What the Scene View shows of a region mark-up: its display (walls and lid) through the gizmo
// and the glow's mesh path, nothing while hidden, and the ground it stands on.

#include "Markups/MarkupDrawList.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupGizmo.h"
#include "Markups/MarkupPathPort.h"
#include "Markups/MarkupPicking.h"
#include "Markups/MarkupRegion/MarkupRegionDisplay.h"
#include "Markups/MarkupRegion/MarkupRegionPort.h"
#include "Markups/MarkupRenderFeature.h"
#include "Markups/MarkupRequests.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Components/Markup/Markup.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupService.h"
#include "Scene/MeshBvhCache.h"
#include "Mathematics/Geometry.h"
#include "Scene/SceneTlas.h"
#include "SceneView/SceneOverlayPick.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewProjection.h"
#include "Scripting/ScriptsConfig.h"
#include "SceneView/SplineTool/SplineDrapePolylines.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainService.h"
#include "TestEnvVar.h"
#include "UndoRedo/UndoRedoService.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace GameEngine;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

constexpr Rendering::ViewId kViewId = 32;
constexpr float kHeight = 8.0f;

class MarkupRegionSceneViewTest : public ::testing::Test
{
  protected:
    // The drape's conform rays pick through the engine's scene picking.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scriptsConfig);
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        MarkupECS::MarkupService::Initialize();
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_Bridge = std::make_unique<Editor::MarkupEditorBridge>(
            m_Notifications, []() { return int64{1000}; }, []() { return std::optional<std::filesystem::path>(); });
    }

    void TearDown() override
    {
        Editor::SceneTools::SetSceneOverlayPicker({});
        Scene::ReleaseSceneTlas(m_World);
        for (const GUID& guid : m_MeshGuids)
            Scene::MeshBvhCache::Instance().InvalidateAsset(guid);
        TearDownTerrain();
        m_Bridge.reset();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
        MarkupECS::MarkupService::Shutdown();
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
    }

    // A linear region over `knots` (world x, z) standing at y = 0, with no terrain under it.
    ECS::EntityHandle CreateRegion(const std::vector<Vector2>& knots)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        const Editor::RegionPlacement placement =
            Editor::NewRegionPlacement(knots, Mathematics::Matrix4x4::Identity(), 0.0f);
        m_World.AddComponentImmediate(entity, placement.Local);
        m_World.AddComponentImmediate(entity, Components::Markup{});
        Editor::AddRegionParts(m_World, entity,
                               placement.World.ToMatrix4x4(placement.World.GetPosition() - Mathematics::WorldPosition{}),
                               knots, Spline::SplineType::Linear, kHeight);
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, entity, Components::MarkupAuthor::User, 1000);
        return entity;
    }

    ECS::EntityHandle CreateBox(const Vector3& center, const Vector3& size)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::Transform local{};
        local.Basis[0].x = size.x;
        local.Basis[1].y = size.y;
        local.Basis[2].z = size.z;
        local.Position = Mathematics::WorldPosition::FromVector3(center);
        Components::WorldTransform placed{};
        placed.Position = local.Position;
        placed.Basis = local.Basis;
        m_World.AddComponentImmediate(entity, local);
        m_World.AddComponentImmediate(entity, placed);
        m_World.AddComponentImmediate(entity, Components::Markup{});
        m_World.AddComponentImmediate(entity, Components::MarkupVolume{});
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, entity, Components::MarkupAuthor::User, 1000);
        return entity;
    }


    void DrawFrame(const Vector3& camera = Vector3(0.0f, 80.0f, -150.0f))
    {
        Editor::SceneTools::ResetGizmoLineGroups(kViewId);
        Editor::SceneTools::ResetGizmoTriangleGroups(kViewId);
        Editor::SceneTools::GizmoRenderContext context(kViewId, kViewId, &camera);
        m_Gizmo.Draw(context, m_World, *m_Bridge, {}, camera, ++m_Frame);
    }

    static std::size_t TriangleCount()
    {
        std::size_t count = 0;
        if (const auto* groups = Editor::SceneTools::GetGizmoTriangleGroups(kViewId))
        {
            for (const auto& group : *groups)
                count += group.vertices.size() / 9;
        }
        return count;
    }

    static std::size_t LineCount()
    {
        std::size_t count = 0;
        if (const auto* groups = Editor::SceneTools::GetGizmoLineGroups(kViewId))
        {
            for (const auto& group : *groups)
                count += group.vertices.size() / 6;
        }
        return count;
    }

    const MarkupECS::MarkupRegionDisplayCache::Entry* Display(ECS::EntityHandle region)
    {
        return m_Bridge->RegionDisplaysOf(m_World).Find(region);
    }

    // A linear path through `points` (world) placed at its first point.
    ECS::EntityHandle CreatePath(const std::vector<Vector3>& points)
    {
        const ECS::EntityHandle path = m_World.CreateEntity();
        const Components::Transform placed = Editor::NewPathPlacement(points);
        m_World.AddComponentImmediate(path, placed);
        m_World.AddComponentImmediate(path, Components::Markup{});
        Editor::AddPathParts(m_World, path, placed.ToMatrix4x4(placed.GetPosition() - Mathematics::WorldPosition{}),
                             points, Spline::SplineType::Linear);
        (void)MarkupECS::MarkupService::Get().BeginMarkup(m_World, path, Components::MarkupAuthor::User, 1000);
        return path;
    }

    // A path's band stands this far above the ground at a sample `distance` meters from the camera:
    // 0.1 % of it, at least 0.05 m (MarkupGizmo's lift rule).
    static float PathLift(float distance) { return std::max(0.05f, 0.001f * distance); }

    // Every vertex of the gizmo's triangles this frame.
    static std::vector<Vector3> TriangleVertices()
    {
        std::vector<Vector3> vertices;
        if (const auto* groups = Editor::SceneTools::GetGizmoTriangleGroups(kViewId))
        {
            for (const auto& group : *groups)
            {
                for (std::size_t i = 0; i + 2 < group.vertices.size(); i += 3)
                    vertices.emplace_back(group.vertices[i], group.vertices[i + 1], group.vertices[i + 2]);
            }
        }
        return vertices;
    }

    // A 400 m terrain centered on the origin holding one cone, kConeHeight tall and kConeRadius
    // across its foot, around the origin; flat at y = 0 beyond it.
    static constexpr float kConeHeight = 300.0f;
    static constexpr float kConeRadius = 150.0f;
    static float Cone(float x, float z) { return std::max(0.0f, 1.0f - std::hypot(x, z) / kConeRadius); }
    void CreateConeTerrain() { CreateTerrainOf(kConeHeight, &Cone); }

    // A 400 m terrain centered on the origin, `heightScale` times `normalized` (0 to 1) high.
    void CreateTerrainOf(float heightScale, float (*normalized)(float x, float z))
    {
        m_TerrainHeightScale = heightScale;
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        TerrainECS::TerrainService& terrains = TerrainECS::TerrainService::Get();
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = 65;
        config.HeightmapHeight = 65;
        config.WorldSizeX = 400.0f;
        config.WorldSizeZ = 400.0f;
        config.HeightScale = heightScale;
        config.LODLevels = 1;
        config.PatchGridSize = 8;
        m_Terrain = terrains.CreateTerrain(config);
        ASSERT_NE(m_Terrain.Generation, 0u);
        TerrainECS::TerrainData* data = terrains.GetTerrainData(m_Terrain);
        ASSERT_NE(data, nullptr);
        float* samples = data->Heightfield.GetMutableSamples();
        const uint32 width = data->Heightfield.GetWidth();
        for (uint32 row = 0; row < data->Heightfield.GetHeight(); ++row)
        {
            for (uint32 column = 0; column < width; ++column)
            {
                const float x = -200.0f + 400.0f * static_cast<float>(column) / static_cast<float>(width - 1);
                const float z = -200.0f + 400.0f * static_cast<float>(row) / static_cast<float>(width - 1);
                samples[row * width + column] = normalized(x, z);
            }
        }
        terrains.RebuildQuadtree(m_Terrain);
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = config.WorldSizeX;
        terrain.SizeZ = config.WorldSizeZ;
        terrain.HeightScale = config.HeightScale;
        terrain.TerrainDataHandle = m_Terrain.Index;
        terrain.TerrainDataGeneration = m_Terrain.Generation;
        m_World.AddComponentImmediate(entity, terrain);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
    }

    // The terrain's own height at world (x, z): its heightfield, bilinear, as the renderer draws it.
    float TerrainGroundAt(float x, float z) const
    {
        const TerrainECS::TerrainData* data = TerrainECS::TerrainService::Get().GetTerrainData(m_Terrain);
        return data->Heightfield.SampleBilinear((x + 200.0f) / 400.0f, (z + 200.0f) / 400.0f) * m_TerrainHeightScale;
    }

    void TearDownTerrain()
    {
        if (m_Terrain.Generation != 0u)
            TerrainECS::TerrainService::Get().DestroyTerrain(m_Terrain);
        m_Terrain = {};
    }

    // The mark-up whose outline the picker finds under the pointer of `view`.
    ECS::EntityHandle PickOutline(const Editor::SceneTools::ScenePointerEvent& view,
                                  const Editor::SceneTools::SceneOverlayPickFilter& isPickable = {})
    {
        return Editor::PickMarkupOutline(m_World, *m_Bridge, view, ++m_Frame, isPickable).Entity;
    }

    // A rendered mesh of the two triangles over the quad `corners` (in order around it), with its
    // bounds, registered with the asset manager as a loaded model.
    ECS::EntityHandle CreateQuadMesh(const std::array<Vector3, 4>& corners)
    {
        Mesh mesh;
        mesh.Vertices.resize(corners.size());
        Vector3 low = corners[0];
        Vector3 high = corners[0];
        for (std::size_t i = 0; i < corners.size(); ++i)
        {
            auto& position = mesh.Vertices[i].Position;
            position[0] = corners[i].x;
            position[1] = corners[i].y;
            position[2] = corners[i].z;
            low = Vector3(std::min(low.x, corners[i].x), std::min(low.y, corners[i].y), std::min(low.z, corners[i].z));
            high = Vector3(std::max(high.x, corners[i].x), std::max(high.y, corners[i].y), std::max(high.z, corners[i].z));
        }
        mesh.Indices = {0u, 1u, 2u, 0u, 2u, 3u};
        const GUID guid = GUID::Generate();
        auto model = MakeShared<ModelAsset>(guid, "markup_pick_quad.glb");
        model->SetMeshesForTest({std::move(mesh)});
        EngineCore::GetInstance().GetAssetManager().RegisterLoadedAsset(guid, model);
        m_MeshGuids.push_back(guid);
        (void)Scene::MeshBvhCache::Instance().GetOrBuild(guid, 0, model->GetMesh(0));

        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::MeshRenderer renderer{};
        renderer.modelAssetGuid.Set(guid);
        m_World.AddComponentImmediate(entity, renderer);
        m_World.AddComponentImmediate(entity, Components::WorldTransform{});
        Components::LocalBounds bounds{};
        bounds.Box.center = (low + high) * 0.5f;
        bounds.Box.halfExtents = (high - low) * 0.5f;
        m_World.AddComponentImmediate(entity, bounds);
        return entity;
    }

    TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("MarkupRegionSceneView")};
    Editor::EditorChangeNotifications m_Notifications;
    ECS::World m_World;
    std::unique_ptr<Editor::MarkupEditorBridge> m_Bridge;
    Editor::MarkupGizmo m_Gizmo;
    uint64 m_Frame = 0;
    bool m_OwnsSplines = false;
    TerrainECS::TerrainHandle m_Terrain{};
    float m_TerrainHeightScale = 0.0f;
    std::vector<GUID> m_MeshGuids;
};

const std::vector<Vector2> kSquare{{0.0f, 0.0f}, {40.0f, 0.0f}, {40.0f, 40.0f}, {0.0f, 40.0f}};

// A 1600 by 900 pixel perspective view, 60 degrees high, from `position` toward `target`: the
// camera state SceneViewController::PopulatePointerCameraState writes (right = up x forward,
// up = forward x right).
Editor::SceneTools::ScenePointerEvent MakeView(const Vector3& position, const Vector3& target)
{
    Editor::SceneTools::ScenePointerEvent view{};
    view.viewW = 1600.0f;
    view.viewH = 900.0f;
    view.cameraPos = position;
    view.cameraForward = (target - position).NormalizeOrZero();
    view.cameraRight = Vector3::Cross(Vector3(0.0f, 1.0f, 0.0f), view.cameraForward).NormalizeOrZero();
    view.cameraUp = Vector3::Cross(view.cameraForward, view.cameraRight).NormalizeOrZero();
    view.tanHalfFovY = 0.57735027f;
    return view;
}

// The orthographic view of MakeView, `height` meters high.
Editor::SceneTools::ScenePointerEvent MakeOrthographicView(const Vector3& position, const Vector3& target, float height)
{
    Editor::SceneTools::ScenePointerEvent view = MakeView(position, target);
    view.tanHalfFovY = 0.0f;
    view.orthoHeight = height;
    return view;
}

// Puts the pointer of `view` `offsetX` and `offsetY` pixels from where `world` projects.
void PointAt(Editor::SceneTools::ScenePointerEvent& view, const Vector3& world, float offsetX, float offsetY)
{
    Vector2 pixel;
    ASSERT_TRUE(Editor::SceneTools::ProjectWorldToView(view, world, pixel));
    view.viewX = pixel.x + offsetX;
    view.viewY = pixel.y + offsetY;
}

} // namespace

// The gizmo fills a region with its display's walls and lid and outlines its ring at the base and
// along the top edge; a hidden region draws nothing and leaves no group behind.
TEST_F(MarkupRegionSceneViewTest, ARegionDrawsItsDisplayAndNothingWhileHidden)
{
    const ECS::EntityHandle region = CreateRegion(kSquare);
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    ASSERT_TRUE(display->Ground.Closed);
    const std::size_t samples = display->Ground.Outline.size();
    EXPECT_GE(samples, 150u) << "the drape samples the 160 m outline about every meter";
    EXPECT_EQ(display->Mesh.WallVertexCount, samples * 6u) << "one wall quad per drape sample";
    EXPECT_GT(display->Mesh.Vertices.size(), display->Mesh.WallVertexCount) << "no lid";
    EXPECT_EQ(TriangleCount(), display->Mesh.Vertices.size() / 3u);
    EXPECT_EQ(LineCount(), samples * 2u) << "the ring at the base and along the top edge";

    m_Bridge->SetHidden(m_World, std::span<const ECS::EntityHandle>(&region, 1), true);
    DrawFrame();
    EXPECT_EQ(TriangleCount(), 0u);
    EXPECT_EQ(LineCount(), 0u);
}

// The glow draws a region from its display's mesh: the mesh shape, every vertex of it.
TEST_F(MarkupRegionSceneViewTest, TheGlowDrawsARegionFromItsMesh)
{
    CreateRegion(kSquare);
    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, {}, Vector3(0.0f, 80.0f, -150.0f), nullptr, 1, items, excludedScratch);
    ASSERT_EQ(items.size(), 1u);
    ASSERT_NE(items[0].Region, nullptr);
    Editor::MarkupRenderFeature glow;
    std::vector<Editor::MarkupRenderFeature::Draw> draws;
    glow.BuildDraws(items, bodies, draws);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].Rim[3], 2.0f);
    EXPECT_EQ(draws[0].VertexCount, items[0].Region->Mesh.Vertices.size());
    EXPECT_EQ(draws[0].Region, items[0].Region);
}

// A path draws its draped line and a band either side of it through the gizmo, a quad of band each
// side of the line and one line per pair of samples, and has no glow body.
TEST_F(MarkupRegionSceneViewTest, APathDrawsItsDrapedLineAndBandWithoutAGlowBody)
{
    const ECS::EntityHandle road = CreatePath({{0.0f, 0.0f, 0.0f}, {60.0f, 0.0f, 10.0f}, {120.0f, 0.0f, 0.0f}});

    DrawFrame();
    const auto* display = Display(road);
    ASSERT_NE(display, nullptr);
    EXPECT_FALSE(display->Ground.Closed);
    const std::size_t samples = display->Ground.Outline.size();
    EXPECT_GE(samples, 100u) << "the drape samples the 121 m path about every meter";
    EXPECT_EQ(TriangleCount(), (samples - 1) * 4u);
    EXPECT_EQ(LineCount(), samples - 1);

    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, {}, Vector3(0.0f, 80.0f, -150.0f), nullptr, 2, items, excludedScratch);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].Path);
    Editor::MarkupRenderFeature glow;
    std::vector<Editor::MarkupRenderFeature::Draw> draws;
    glow.BuildDraws(items, bodies, draws);
    EXPECT_TRUE(draws.empty()) << "a path drew a glow body";
}

// Where the ground is below the sea, a region stands on the sea surface: an ocean at y = 5 lifts
// every wall's foot and the lid to it.
TEST_F(MarkupRegionSceneViewTest, ARegionStandsOnTheSeaWhereTheGroundIsBelowIt)
{
    const ECS::EntityHandle ocean = m_World.CreateEntity();
    Components::WorldTransform placed{};
    placed.Position.Y = 5.0;
    m_World.AddComponentImmediate(ocean, placed);
    m_World.AddComponentImmediate(ocean, Components::OceanSurface{});
    EXPECT_FLOAT_EQ(Editor::ResolveMarkupSeaLevel(m_World), 5.0f);

    const ECS::EntityHandle region = CreateRegion(kSquare);
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    for (const Vector3& sample : display->Ground.Outline)
        EXPECT_GE(sample.y, 5.0f);
    for (const Vector3& point : display->Ground.LidPoints)
        EXPECT_GE(point.y, 5.0f);
}

// A region whose outline climbs a hill taller than the drape's 100 m lift stands on the hill: its
// walls' feet and its lid follow the terrain wherever it is, since the ground rays start above the
// terrain, not a fixed height above the outline (whose knots stand at the region's label point).
TEST_F(MarkupRegionSceneViewTest, ARegionOverAHillTallerThanTheRayLiftStandsOnTheHill)
{
    CreateConeTerrain();
    // Its edges cross the cone 60 m from the peak, 180 m up.
    const ECS::EntityHandle region =
        CreateRegion({{-60.0f, -60.0f}, {60.0f, -60.0f}, {60.0f, 60.0f}, {-60.0f, 60.0f}});
    DrawFrame();
    const auto* display = Display(region);
    ASSERT_NE(display, nullptr);
    float highest = 0.0f;
    for (const Vector3& sample : display->Ground.Outline)
    {
        const float ground = TerrainGroundAt(sample.x, sample.z);
        highest = std::max(highest, ground);
        EXPECT_NEAR(sample.y, ground + Editor::SceneTools::kDrapeSurfaceLiftMetres, 0.5f)
            << "a wall's foot off the ground at (" << sample.x << ", " << sample.z << ")";
    }
    EXPECT_GT(highest, 150.0f) << "the outline never climbs above the ray's old reach";
    for (const Vector3& point : display->Ground.LidPoints)
        EXPECT_NEAR(point.y, TerrainGroundAt(point.x, point.z) + Editor::SceneTools::kDrapeSurfaceLiftMetres, 0.5f)
            << "the lid off the ground at (" << point.x << ", " << point.z << ")";
}

// markup_get gives a region's groundRange, the lowest and highest heights it stands on, built for
// a region no view has drawn.
TEST_F(MarkupRegionSceneViewTest, GetGivesTheGroundRangeOfARegionNoViewDrew)
{
    CreateConeTerrain();
    const ECS::EntityHandle region =
        CreateRegion({{-60.0f, -60.0f}, {60.0f, -60.0f}, {60.0f, 60.0f}, {-60.0f, 60.0f}});
    ASSERT_EQ(Display(region), nullptr);
    Editor::UndoRedoService undo;
    Editor::MarkupRequestContext context;
    context.World = &m_World;
    context.Bridge = m_Bridge.get();
    context.Undo = &undo;
    context.Notifications = &m_Notifications;
    const nlohmann::json shape = Editor::GetMarkup(context, {{"entityId", region.id}})["markup"]["shape"];
    ASSERT_TRUE(shape.contains("groundRange")) << shape.dump();

    const auto* display = Display(region);
    ASSERT_NE(display, nullptr) << "the ground was not built";
    float lowest = std::numeric_limits<float>::max();
    float highest = std::numeric_limits<float>::lowest();
    for (const auto* points : {&display->Ground.Outline, &display->Ground.LidPoints})
    {
        for (const Vector3& point : *points)
        {
            lowest = std::min(lowest, point.y);
            highest = std::max(highest, point.y);
        }
    }
    EXPECT_FLOAT_EQ(shape["groundRange"]["min"].get<float>(), lowest);
    EXPECT_FLOAT_EQ(shape["groundRange"]["max"].get<float>(), highest);
    EXPECT_GT(highest, 150.0f);
}

// A path along a cross slope lies on it across its whole width: the band's edges stand on the
// ground beside the line, not at the line's height (where the downhill edge would float and the
// uphill one sink).
TEST_F(MarkupRegionSceneViewTest, APathBandLiesOnACrossSlope)
{
    // 100 m over the 400 m terrain along x: a 0.25 cross slope under a path running along z.
    CreateTerrainOf(100.0f, [](float x, float) { return (x + 200.0f) / 400.0f; });
    CreatePath({{0.0f, 50.0f, -15.0f}, {0.0f, 50.0f, 15.0f}});
    const Vector3 camera(0.0f, 60.0f, -40.0f); // every sample within 125 m: the band's 1.5 m minimum
    DrawFrame(camera);
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float widest = 0.0f;
    for (const Vector3& vertex : band)
    {
        widest = std::max(widest, std::fabs(vertex.x));
        const Vector3 sample(0.0f, TerrainGroundAt(0.0f, vertex.z), vertex.z);
        EXPECT_NEAR(vertex.y - TerrainGroundAt(vertex.x, vertex.z), PathLift((sample - camera).Length()), 0.02f)
            << "a band vertex off the ground at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_NEAR(widest, 1.5f, 0.01f) << "the band is not 1.5 m to either side";
}

// A path's band widens and lifts with each sample's own distance from the camera: 1.5 m to either
// side and 0.05 m up near it, 1.2 % and 0.1 % of the distance at the far end of the same path.
TEST_F(MarkupRegionSceneViewTest, APathBandWidensWithDistancePerSample)
{
    CreatePath({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1000.0f}});
    const Vector3 camera(0.0f, 2.0f, -5.0f);
    DrawFrame(camera);
    bool sawNear = false;
    bool sawFar = false;
    for (const Vector3& vertex : TriangleVertices())
    {
        if (std::fabs(vertex.x) < 1.0e-3f)
            continue; // the line's own vertices
        if (vertex.z < 1.0e-3f)
        {
            sawNear = true;
            EXPECT_NEAR(std::fabs(vertex.x), 1.5f, 1.0e-3f);
            EXPECT_NEAR(vertex.y, 0.05f, 1.0e-3f);
        }
        if (vertex.z > 1000.0f - 1.0e-3f)
        {
            sawFar = true;
            const float distance = (Vector3(0.0f, 0.0f, 1000.0f) - camera).Length();
            EXPECT_NEAR(std::fabs(vertex.x), 0.012f * distance, 0.01f) << "the far end keeps the near width";
            EXPECT_NEAR(vertex.y, 0.001f * distance, 0.01f) << "the far end's lift is not 0.1 % of its distance";
        }
    }
    EXPECT_TRUE(sawNear && sawFar);
}

// From about 500 m the band is about 6 m to either side, so its edges stand on the ground read
// between the stored offsets (4 and 10 m), not at the narrowest one: every band vertex still lies
// its lift above the ground under it on the cross slope.
TEST_F(MarkupRegionSceneViewTest, APathBandLiesOnACrossSlopeFromFarAway)
{
    CreateTerrainOf(100.0f, [](float x, float) { return (x + 200.0f) / 400.0f; });
    CreatePath({{0.0f, 50.0f, -15.0f}, {0.0f, 50.0f, 15.0f}});
    const Vector3 camera(0.0f, 60.0f, -515.0f);
    DrawFrame(camera);
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float widest = 0.0f;
    for (const Vector3& vertex : band)
    {
        widest = std::max(widest, std::fabs(vertex.x));
        // The lift follows the distance of the line's sample the vertex belongs to.
        const Vector3 sample(0.0f, TerrainGroundAt(0.0f, vertex.z), vertex.z);
        EXPECT_NEAR(vertex.y - TerrainGroundAt(vertex.x, vertex.z), PathLift((sample - camera).Length()), 0.02f)
            << "a band vertex off the ground at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_GT(widest, 5.5f) << "the band is not wider than the narrowest stored offset";
}

// A path over a ridge between its two knots stays on the ridge: the drape samples the ground about
// every meter, so the line and the band follow the rise and fall between the knots rather than the
// straight segment under them (up to 20 m off here). A guard: it holds at the change that adds it,
// whichever lift the band uses, so it bounds the band within a meter above the ground.
TEST_F(MarkupRegionSceneViewTest, APathOverARidgeBetweenTwoKnotsStaysOnIt)
{
    // A 20 m ridge across the path at x = 0 (a sample every meter over the 400 m terrain).
    CreateTerrainOf(20.0f, [](float x, float) { return std::max(0.0f, 1.0f - std::fabs(x) / 10.0f); });
    CreatePath({{-20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}});
    DrawFrame(Vector3(0.0f, 30.0f, -40.0f));
    const std::vector<Vector3> band = TriangleVertices();
    ASSERT_FALSE(band.empty());
    float highest = 0.0f;
    for (const Vector3& vertex : band)
    {
        const float ground = TerrainGroundAt(vertex.x, vertex.z);
        highest = std::max(highest, ground);
        EXPECT_GE(vertex.y - ground, 0.0f)
            << "a band vertex under the ridge at (" << vertex.x << ", " << vertex.z << ")";
        EXPECT_LE(vertex.y - ground, 1.0f)
            << "a band vertex off the ridge at (" << vertex.x << ", " << vertex.z << ")";
    }
    EXPECT_GT(highest, 19.0f) << "the band never reaches the ridge's top";
}

// A click a few pixels off a region's outline, at its base or along its top edge, picks the
// region; a click inside it or well off the outline picks nothing, so the ground it encloses keeps
// its clicks.
TEST_F(MarkupRegionSceneViewTest, AClickOnARegionsOutlinePicksTheRegion)
{
    const ECS::EntityHandle region = CreateRegion(kSquare);
    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(20.0f, 60.0f, -50.0f), Vector3(20.0f, 0.0f, 20.0f));

    PointAt(view, Vector3(20.0f, 0.0f, 0.0f), 0.0f, 3.0f);
    EXPECT_EQ(PickOutline(view), region) << "the near edge's base";
    PointAt(view, Vector3(30.0f, kHeight, 40.0f), 0.0f, -3.0f);
    EXPECT_EQ(PickOutline(view), region) << "the far edge's top";
    PointAt(view, Vector3(0.0f, 0.0f, 20.0f), -3.0f, 0.0f);
    EXPECT_EQ(PickOutline(view), region) << "the left edge's base";

    PointAt(view, Vector3(20.0f, 0.0f, 20.0f), 0.0f, 0.0f);
    EXPECT_FALSE(PickOutline(view).IsValid()) << "inside the region";
    PointAt(view, Vector3(20.0f, 0.0f, 0.0f), 0.0f, 40.0f);
    EXPECT_FALSE(PickOutline(view).IsValid()) << "40 px off the edge";
}

// A path picks along its draped line.
TEST_F(MarkupRegionSceneViewTest, AClickOnAPathsLinePicksThePath)
{
    const ECS::EntityHandle road = CreatePath({{0.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, {60.0f, 0.0f, 0.0f}});
    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(30.0f, 40.0f, -40.0f), Vector3(30.0f, 0.0f, 0.0f));

    PointAt(view, Vector3(30.0f, 0.0f, 0.0f), 0.0f, -3.0f);
    EXPECT_EQ(PickOutline(view), road);
    PointAt(view, Vector3(30.0f, 0.0f, 0.0f), 0.0f, 40.0f);
    EXPECT_FALSE(PickOutline(view).IsValid());
}

// A box picks along its edges, not across its faces.
TEST_F(MarkupRegionSceneViewTest, AClickOnABoxsEdgePicksTheBox)
{
    const ECS::EntityHandle box = CreateBox(Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));
    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(0.0f, 4.0f, -8.0f), Vector3(0.0f, 1.0f, 0.0f));

    PointAt(view, Vector3(0.0f, 2.0f, -1.0f), 0.0f, 3.0f);
    EXPECT_EQ(PickOutline(view), box) << "the near top edge";
    PointAt(view, Vector3(0.0f, 1.3f, -1.0f), 0.0f, 0.0f);
    EXPECT_FALSE(PickOutline(view).IsValid()) << "the near face";
}

// A hidden region, and one the Scene View refuses (a locked one), pick nothing.
TEST_F(MarkupRegionSceneViewTest, AHiddenOrRefusedRegionPicksNothing)
{
    const ECS::EntityHandle region = CreateRegion(kSquare);
    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(20.0f, 60.0f, -50.0f), Vector3(20.0f, 0.0f, 20.0f));
    PointAt(view, Vector3(20.0f, 0.0f, 0.0f), 0.0f, 3.0f);
    ASSERT_EQ(PickOutline(view), region);

    const auto refuseAll = [](ECS::EntityHandle) { return false; };
    EXPECT_FALSE(PickOutline(view, refuseAll).IsValid());

    m_Bridge->SetHidden(m_World, std::span<const ECS::EntityHandle>(&region, 1), true);
    EXPECT_FALSE(PickOutline(view).IsValid());
}

// A box seen from inside a perspective view picks along an edge that runs from behind the camera to
// in front of it, on the part in front.
TEST_F(MarkupRegionSceneViewTest, AnEdgePassingBehindAPerspectiveCameraPicksInFront)
{
    const ECS::EntityHandle box = CreateBox(Vector3(0.0f, 1.0f, 10.0f), Vector3(4.0f, 2.0f, 40.0f));
    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(0.0f, 3.0f, 0.0f), Vector3(0.0f, 1.0f, 10.0f));

    PointAt(view, Vector3(-2.0f, 2.0f, 10.0f), 0.0f, 3.0f);
    EXPECT_EQ(PickOutline(view), box);
}

// An orthographic view picks nothing of an outline wholly behind its camera, which projects onto
// the view all the same.
TEST_F(MarkupRegionSceneViewTest, AnOutlineBehindAnOrthographicCameraPicksNothing)
{
    (void)CreateBox(Vector3(0.0f, 1.0f, -20.0f), Vector3(2.0f, 2.0f, 2.0f));
    Editor::SceneTools::ScenePointerEvent view =
        MakeOrthographicView(Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 1.0f, 10.0f), 20.0f);

    PointAt(view, Vector3(0.0f, 2.0f, -21.0f), 0.0f, 0.0f);
    EXPECT_FALSE(PickOutline(view).IsValid());
}

// Of two outlines that overlap on screen, the one nearer the camera picks, from either side.
TEST_F(MarkupRegionSceneViewTest, OfTwoOverlappingOutlinesTheNearerPicks)
{
    const ECS::EntityHandle south = CreateBox(Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));
    const ECS::EntityHandle north = CreateBox(Vector3(0.0f, 1.0f, 10.0f), Vector3(2.0f, 2.0f, 2.0f));

    Editor::SceneTools::ScenePointerEvent fromSouth =
        MakeOrthographicView(Vector3(0.0f, 1.0f, -20.0f), Vector3(0.0f, 1.0f, 0.0f), 20.0f);
    PointAt(fromSouth, Vector3(0.0f, 2.0f, 0.0f), 0.0f, 0.0f);
    EXPECT_EQ(PickOutline(fromSouth), south);

    Editor::SceneTools::ScenePointerEvent fromNorth =
        MakeOrthographicView(Vector3(0.0f, 1.0f, 30.0f), Vector3(0.0f, 1.0f, 0.0f), 20.0f);
    PointAt(fromNorth, Vector3(0.0f, 2.0f, 10.0f), 0.0f, 0.0f);
    EXPECT_EQ(PickOutline(fromNorth), north);
}

// A sphere picks along its silhouette, not across its face; from inside it, along its rings.
TEST_F(MarkupRegionSceneViewTest, ASpherePicksAlongItsSilhouetteAndFromInsideAlongItsRings)
{
    const ECS::EntityHandle ball = CreateBox(Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 4.0f, 4.0f));
    m_World.GetComponentForWrite<Components::MarkupVolume>(ball)->Shape = Components::MarkupVolumeShape::Sphere;
    Editor::SceneTools::ScenePointerEvent outside = MakeView(Vector3(0.0f, 1.0f, -10.0f), Vector3(0.0f, 1.0f, 0.0f));

    PointAt(outside, Vector3(0.0f, 3.0f, 0.0f), 0.0f, -3.0f);
    EXPECT_EQ(PickOutline(outside), ball) << "the silhouette's top";
    PointAt(outside, Vector3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f);
    EXPECT_FALSE(PickOutline(outside).IsValid()) << "the sphere's face";

    const ECS::EntityHandle dome = CreateBox(Vector3(0.0f, 0.0f, 100.0f), Vector3(20.0f, 20.0f, 20.0f));
    m_World.GetComponentForWrite<Components::MarkupVolume>(dome)->Shape = Components::MarkupVolumeShape::Sphere;
    Editor::SceneTools::ScenePointerEvent inside = MakeView(Vector3(0.0f, 0.0f, 97.0f), Vector3(0.0f, 0.0f, 110.0f));
    PointAt(inside, Vector3(5.0f, 0.0f, 100.0f + 8.660254f), 0.0f, -3.0f);
    EXPECT_EQ(PickOutline(inside), dome) << "the level ring ahead";
}

// An outline that scene geometry hides leaves the click to that geometry: a box's edge behind a
// wall picks nothing through the wall and picks where it clears it. A region's ring drawn on the
// floor it stands on stays in sight from either side of it.
TEST_F(MarkupRegionSceneViewTest, AnOutlineHiddenBehindAMeshDoesNotPick)
{
    const ECS::EntityHandle box = CreateBox(Vector3(0.0f, 1.0f, 5.0f), Vector3(4.0f, 2.0f, 2.0f));
    (void)CreateQuadMesh({Vector3(-3.0f, 0.0f, 0.0f), Vector3(-3.0f, 4.0f, 0.0f), Vector3(0.0f, 4.0f, 0.0f),
                          Vector3(0.0f, 0.0f, 0.0f)});
    const ECS::EntityHandle region = CreateRegion(kSquare);
    (void)CreateQuadMesh({Vector3(-10.0f, 0.0f, -10.0f), Vector3(-10.0f, 0.0f, 50.0f), Vector3(50.0f, 0.0f, 50.0f),
                          Vector3(50.0f, 0.0f, -10.0f)});
    Editor::SceneTools::SetSceneOverlayPicker(
        [this](ECS::World& world, const Editor::SceneTools::ScenePointerEvent& event,
               const Editor::SceneTools::SceneOverlayPickFilter& isPickable)
        { return Editor::PickMarkupOutline(world, *m_Bridge, event, ++m_Frame, isPickable); });
    Scene::GetSceneTlas(m_World).WaitCurrent(m_World);

    Editor::SceneTools::ScenePointerEvent view = MakeView(Vector3(0.0f, 2.0f, -10.0f), Vector3(0.0f, 2.0f, 5.0f));
    PointAt(view, Vector3(-1.0f, 2.0f, 4.0f), 0.0f, 3.0f);
    ASSERT_EQ(PickOutline(view), box) << "the picker found no edge to hide";
    EXPECT_FALSE(Editor::SceneTools::PickSceneOverlay(m_World, view, {}).IsValid()) << "picked through the wall";
    PointAt(view, Vector3(1.0f, 2.0f, 4.0f), 0.0f, 3.0f);
    EXPECT_EQ(Editor::SceneTools::PickSceneOverlay(m_World, view, {}), box) << "beside the wall";

    Editor::SceneTools::ScenePointerEvent above =
        MakeView(Vector3(20.0f, 60.0f, -50.0f), Vector3(20.0f, 0.0f, 20.0f));
    PointAt(above, Vector3(20.0f, 0.0f, 0.0f), 0.0f, 3.0f);
    EXPECT_EQ(Editor::SceneTools::PickSceneOverlay(m_World, above, {}), region) << "below the ring";
    PointAt(above, Vector3(20.0f, 0.0f, 0.0f), 0.0f, -3.0f);
    EXPECT_EQ(Editor::SceneTools::PickSceneOverlay(m_World, above, {}), region) << "above the ring";
}
