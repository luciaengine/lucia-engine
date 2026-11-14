#include "DayCycleEditorTestFixture.h"

#include "Assets/EditorAssetRegistrations.h"
#include "Core/Engine.h"
#include "DayCycle/DayCycleWindow/DayCycleWindow.h"
#include "UI/Controls/Label.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <chrono>
#include <thread>

namespace GameEngine::Editor
{
class DayCycleWindowBindingTests : public ::testing::Test
{
  protected:
    std::filesystem::path Root;

    void SetUp() override
    {
        DayCycleWorkingCopy::CloseAll();
        Root = TestUtils::MakeUniqueTempDirectory("ge_daycycle_bound_window");
        const auto editorRoot = Root / "Editor";
        std::filesystem::create_directories(Root / "Project");
        std::filesystem::create_directories(editorRoot / "UI/panels");
        std::filesystem::create_directories(editorRoot / "DayCycles");
        for (const char* relative : {"UI/panels/DayCycleWindow.uxml", "DayCycles/Default.daycycle.json"})
            std::filesystem::copy_file(TestPaths::ExecutableDirectory() / "Assets" / relative, editorRoot / relative);

        EngineCore& engine = EngineCore::GetInstance();
        ASSERT_FALSE(engine.IsInitialized());
        ApplicationConfig config{};
        config.AssetDirectory = (Root / "Project").string();
        config.WorkspaceDirectory = Root.string();
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
        RegisterEditorAssetTypes(engine.GetAssetManager().GetAssetTypeRegistry());
        AssetSourceDesc editor{};
        editor.Alias = "editor";
        editor.Root = editorRoot;
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(editor));
        engine.GetAssetManager().WaitForStartupScan("editor");
    }

    void TearDown() override
    {
        DayCycleWorkingCopy::CloseAll();
        EngineCore::GetInstance().Shutdown();
        std::error_code error;
        std::filesystem::remove_all(Root, error);
    }
};

TEST_F(DayCycleWindowBindingTests, WorkingCopyRenameRefreshesTheMountedTitleAndUndo)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    UndoRedoService undo;
    DayCycleEditorServices services;
    services.Assets = &assets;
    services.Undo = &undo;
    {
        UIManager manager{nullptr, &assets};
        auto owned = std::make_unique<DayCycleWindow>(services);
        DayCycleWindow* window = owned.get();
        manager.SetRoot(std::move(owned));
        UILayoutAccess::SetLastLayoutRect(*window, 0.0f, 0.0f, 900.0f, 600.0f);
        window->OnPostLayout();
        Label* title = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            manager.GetDispatcher()->Drain();
            window->Refresh();
            title = dynamic_cast<Label*>(window->FindById("DayCycleProfileName"_sid));
            if (title && title->GetText() == "Default")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_NE(title, nullptr) << "the production window layout did not bind";
        ASSERT_EQ(title->GetText(), "Default");
        const auto profile = window->GetProfile();
        ASSERT_NE(profile, nullptr);
        DayCycleProfileData renamed = profile->GetData();
        renamed.Name = "Dusk profile";
        profile->Commit(renamed, "Rename profile", &undo);
        window->Refresh();
        EXPECT_EQ(title->GetText(), "Dusk profile");
        undo.Undo();
        window->Refresh();
        EXPECT_EQ(title->GetText(), "Default");
        undo.Redo();
        window->Refresh();
        EXPECT_EQ(title->GetText(), "Dusk profile");
    }
}
} // namespace GameEngine::Editor
