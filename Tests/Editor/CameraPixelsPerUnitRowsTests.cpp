// The pixel-perfect camera's Pixels Per Unit rows: the Auto toggle, the read-only density it
// renders at and its source, and the typed value in manual mode.

#include "Components/Rendering/Camera.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/CameraInspector.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <vector>

using namespace GameEngine;

namespace
{

template <typename T>
void CollectAll(UIElement& element, std::vector<T*>& out)
{
    if (auto* match = dynamic_cast<T*>(&element))
        out.push_back(match);
    for (const auto& child : element.GetChildren())
        CollectAll(*child, out);
}

template <typename T>
std::vector<T*> All(UIElement& root)
{
    std::vector<T*> out;
    CollectAll(root, out);
    return out;
}

bool HasLabel(UIElement& root, std::string_view text)
{
    for (Label* label : All<Label>(root))
        if (label->GetText() == text)
            return true;
    return false;
}

template <typename Asset>
bool WriteAsset(const Asset& asset, const std::filesystem::path& path)
{
    Vector<uint8> bytes;
    if (!asset.SaveToData(bytes))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

class CameraPixelsPerUnitRowsTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(TileMapECS::TileMapRuntimeStore::IsInitialized());
        TileMapECS::TileMapRuntimeStore::Initialize();
        m_Directory = std::filesystem::temp_directory_path() / ("CameraPixelsPerUnitRows_" + m_MapGuid.ToString());
        std::filesystem::create_directories(m_Directory);

        Components::Camera camera{};
        camera.Perspective = false;
        camera.PixelPerfect = true;
        camera.PixelPerfectPixelsPerUnit = 32;
        m_Camera = m_World.CreateEntity();
        m_World.AddComponentImmediate(m_Camera, camera);
    }

    void TearDown() override
    {
        TileMapECS::TileMapRuntimeStore::Shutdown();
        std::error_code error;
        std::filesystem::remove_all(m_Directory, error);
    }

    // Adds a visible layer of 16 px tiles on one-unit cells to the world.
    void AddTileMap()
    {
        const GUID tileSetGuid = GUID::Generate();
        TileMap::TileSetAsset tileSet(tileSetGuid, m_Directory / "grid.tileset");
        tileSet.SetAtlasTexture(GUID::Generate(), 32, 32);
        ASSERT_TRUE(tileSet.SliceGrid(16, 16, 0, 0));
        ASSERT_TRUE(WriteAsset(tileSet, m_Directory / "grid.tileset"));
        TileMap::TileMapAsset map(m_MapGuid, m_Directory / "grid.tilemap");
        const uint32 layerId = map.Data().AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
        map.Data().FindLayer(layerId)->TileSetGuid = tileSetGuid;
        ASSERT_TRUE(WriteAsset(map, m_Directory / "grid.tilemap"));
        auto& store = TileMapECS::TileMapRuntimeStore::Get();
        ASSERT_TRUE(store.LoadMapFromFile(m_MapGuid, m_Directory / "grid.tilemap"));
        ASSERT_TRUE(store.LoadTileSetFromFile(tileSetGuid, m_Directory / "grid.tileset"));

        Components::TileMapLayerRef layer{};
        layer.TileMap.Set(m_MapGuid);
        layer.LayerId = layerId;
        m_World.AddComponentImmediate(m_World.CreateEntity(), layer);
    }

    void Build()
    {
        InspectorContext context{};
        context.Parent = &m_Root;
        context.World = &m_World;
        context.Entity = m_Camera;
        context.Undo = &m_Undo;
        context.ChangeNotifications = &m_Notifications;
        context.FrameRefreshCallbacks = &m_FrameRefresh;
        AddCameraPixelsPerUnitRows(context, [this]() { ++m_RebuildRequests; });
    }

    const Components::Camera& Camera() { return *m_World.GetComponent<Components::Camera>(m_Camera); }

    GUID m_MapGuid = GUID::Generate();
    std::filesystem::path m_Directory;
    ECS::World m_World;
    ECS::EntityHandle m_Camera;
    Editor::UndoRedoService m_Undo;
    Editor::EditorChangeNotifications m_Notifications;
    std::vector<std::function<void()>> m_FrameRefresh;
    UIElement m_Root;
    int m_RebuildRequests = 0;
};

} // namespace

TEST_F(CameraPixelsPerUnitRowsTests, AutoShowsTheTileSetDensityAndItsSource)
{
    AddTileMap();
    Build();
    EXPECT_TRUE(HasLabel(m_Root, "Auto Pixels Per Unit"));
    EXPECT_TRUE(HasLabel(m_Root, "16 (from tile set)"));
    EXPECT_TRUE(All<IntField>(m_Root).empty()) << "Auto mode shows no typed field";
}

TEST_F(CameraPixelsPerUnitRowsTests, AutoWithoutATileMapShowsTheTypedValueAndFollowsANewTileMap)
{
    Build();
    EXPECT_TRUE(HasLabel(m_Root, "32 (typed value)"));
    AddTileMap();
    ASSERT_EQ(m_FrameRefresh.size(), 1u);
    m_FrameRefresh.front()();
    EXPECT_TRUE(HasLabel(m_Root, "16 (from tile set)"));
}

TEST_F(CameraPixelsPerUnitRowsTests, ManualShowsTheTypedValueInAnEditableField)
{
    AddTileMap();
    Components::Camera manual = Camera();
    manual.PixelPerfectAutoPixelsPerUnit = false;
    m_World.AddComponentImmediate(m_Camera, manual);
    Build();
    const std::vector<IntField*> fields = All<IntField>(m_Root);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.front()->GetValue(), 32);
    EXPECT_FALSE(HasLabel(m_Root, "16 (from tile set)"));
}

TEST_F(CameraPixelsPerUnitRowsTests, TheToggleIsOneUndoStepAndAsksForARebuild)
{
    Build();
    const std::vector<Toggle*> toggles = All<Toggle>(m_Root);
    ASSERT_EQ(toggles.size(), 1u);
    ASSERT_TRUE(toggles.front()->IsChecked());

    toggles.front()->SetValue(false);
    EXPECT_FALSE(Camera().PixelPerfectAutoPixelsPerUnit);
    EXPECT_EQ(m_RebuildRequests, 1);
    ASSERT_EQ(m_Undo.GetUndoCount(), 1u);

    m_Undo.Undo();
    EXPECT_TRUE(Camera().PixelPerfectAutoPixelsPerUnit);
}
