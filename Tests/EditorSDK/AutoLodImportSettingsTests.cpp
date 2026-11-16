// The project's automatic LOD import policy: its default, its store and its push to the engine import
// path. The Asset Import page's reset and the project-open apply both read kDefault from here.

#include "Editor/Settings/AutoLodImportSettings.h"

#include "Assets/MeshLODGenerator.h"
#include "Editor/Settings/SettingsStore.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
namespace ed = GameEngine::Editor;
namespace fs = std::filesystem;

class AutoLodImportSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_auto_lod_import_settings");
        fs::create_directories(m_Root / ".Editor");
        m_Saved = GameEngine::GetLODImportSettings();
    }

    void TearDown() override
    {
        GameEngine::SetLODImportSettings(m_Saved);
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    bool EngineGeneratesLods() const { return GameEngine::GetLODImportSettings().AutoGenerateOnImport; }

    fs::path m_Root;
    GameEngine::LODImportSettings m_Saved;
};

TEST_F(AutoLodImportSettingsTest, AProjectWithNoStoredValueGeneratesLods)
{
    EXPECT_TRUE(ed::AutoLodImportSettings::kDefault);
    EXPECT_EQ(ed::AutoLodImportSettings::Load(m_Root), ed::AutoLodImportSettings::kDefault);

    GameEngine::LODImportSettings off = GameEngine::GetLODImportSettings();
    off.AutoGenerateOnImport = false;
    GameEngine::SetLODImportSettings(off);
    ed::AutoLodImportSettings::ApplySaved(m_Root);
    EXPECT_TRUE(EngineGeneratesLods()) << "opening a project with no stored value turns LODs on";
}

TEST_F(AutoLodImportSettingsTest, SaveStoresTheValueAndAppliesIt)
{
    ed::AutoLodImportSettings::SaveAndApply(m_Root, false);
    EXPECT_FALSE(ed::AutoLodImportSettings::Load(m_Root));
    EXPECT_FALSE(EngineGeneratesLods());

    // What a double-click on the row's label does: back to the default, stored and applied.
    ed::AutoLodImportSettings::SaveAndApply(m_Root, ed::AutoLodImportSettings::kDefault);
    EXPECT_TRUE(ed::AutoLodImportSettings::Load(m_Root));
    EXPECT_TRUE(EngineGeneratesLods());
    bool stored = false;
    ed::SettingsStore store = ed::OpenProjectSettings(m_Root);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    ASSERT_TRUE(store.TryGetBool("import.autoGenerateLODs", stored));
    EXPECT_TRUE(stored);
}
} // namespace
