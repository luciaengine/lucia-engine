#include "Core/ApplicationPaths.h"
// The localization package's editor module: the Localization panel is registered with an icon
// class, the sheet the module contributes to the editor's cascade gives that class an image the
// package ships, and the panel has one entry point, the panel menus (Window and the toolbar's
// panel list), like every other panel.

#include "LocalizationPanel.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "TestTempDir.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UI/UIManager.h"
#include <chrono>
#include <thread>

#include "Core/Application.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/Parsers/CSSParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

using GameEngine::Editor::EditorMenuRegistry;
using GameEngine::Editor::EditorPanelDescriptor;
using GameEngine::Editor::EditorPanelRegistry;
using GameEngine::Editor::LocalizationPanel;

namespace
{

// The package's assets, staged beside the suite under the package's mount alias.
std::filesystem::path StagedPackageAssets()
{
    return GameEngine::ApplicationPaths::GetExecutableDirectory() / "LocalizationEditorFixtures" /
           LocalizationPanel::kAssetSourceAlias;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST(LocalizationEditorModule, RegistersThePanelWithItsIconClass)
{
    EditorPanelDescriptor descriptor;
    ASSERT_TRUE(EditorPanelRegistry::Get().TryGetPanel(LocalizationPanel::kPanelId, descriptor));
    EXPECT_EQ(descriptor.Title, "Localization");
    EXPECT_EQ(descriptor.TabIconClass, LocalizationPanel::kTabIconClass);
    EXPECT_EQ(descriptor.DefaultDockAnchorPanelId, "SceneView");
}

TEST(LocalizationEditorModule, ContributesTheSheetThatDeclaresTheIconClass)
{
    const auto sheets = EditorPanelRegistry::Get().StyleSheetSnapshot();
    const bool contributed = std::any_of(sheets.begin(), sheets.end(), [](const auto& sheet) {
        return sheet.AssetSourceAlias == LocalizationPanel::kAssetSourceAlias &&
               sheet.StyleAssetPath == LocalizationPanel::kChromeStyleAssetPath;
    });
    EXPECT_TRUE(contributed);
}

// The image the panel menus read for the panel's row: the class's background-image in the
// contributed sheet, naming a file the package ships under its own mount.
TEST(LocalizationEditorModule, TheIconClassNamesAnIconThePackageShips)
{
    const std::string css = ReadText(StagedPackageAssets() / LocalizationPanel::kChromeStyleAssetPath);
    ASSERT_FALSE(css.empty()) << "the chrome sheet is not staged beside the suite";
    auto sheet = std::make_shared<GameEngine::Stylesheet>();
    ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, *sheet));
    const GameEngine::StylesheetHandle sheets[] = {sheet};

    const std::string url = GameEngine::UIStyleQuery::ResolveClassBackgroundImageUrl(sheets, LocalizationPanel::kTabIconClass);
    const std::string alias = std::string(LocalizationPanel::kAssetSourceAlias) + ":";
    ASSERT_EQ(url.rfind(alias, 0), 0u) << "the icon is not under the package's mount: " << url;
    EXPECT_TRUE(std::filesystem::is_regular_file(StagedPackageAssets() / url.substr(alias.size()))) << url;
}

// One entry point: the panel opens from the panel menus, so the module adds no menu item.
TEST(LocalizationEditorModule, AddsNoMenuItemOfItsOwn)
{
    for (const auto& item : EditorMenuRegistry::Get().Snapshot())
        EXPECT_EQ(item.Path.find("Localization"), std::string::npos) << item.Path;
}

// Every image a package sheet names exists: url("localization:...") under the package's staged
// assets, url("editor:...") under the editor's assets. A missing file draws an empty box.
TEST(LocalizationEditorModule, EverySheetUrlNamesAnExistingFile)
{
    static const std::regex pattern(R"RX(url\("([a-z-]+):([^"]+)"\))RX");
    int checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(StagedPackageAssets()))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".css")
            continue;
        const std::string css = ReadText(entry.path());
        for (auto it = std::sregex_iterator(css.begin(), css.end(), pattern); it != std::sregex_iterator(); ++it)
        {
            const std::string alias = (*it)[1].str();
            const std::string relative = (*it)[2].str();
            std::filesystem::path file;
            if (alias == LocalizationPanel::kAssetSourceAlias)
                file = StagedPackageAssets() / relative;
            else if (alias == "editor")
                file = StagedPackageAssets().parent_path() / "editor" / relative;
            else
                ADD_FAILURE() << entry.path().filename() << " names an unknown mount: " << (*it)[0].str();
            if (!file.empty())
                EXPECT_TRUE(std::filesystem::is_regular_file(file))
                    << entry.path().filename() << " names " << (*it)[0].str() << ", which does not exist";
            ++checked;
        }
    }
    EXPECT_GT(checked, 0) << "no package sheet is staged beside the suite";
}

namespace
{
// Real staged layouts and asset binding, with no window or render/audio/input device.
class LocalizationPanelWorkflow : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        Workspace = GameEngine::TestUtils::MakeUniqueTempDirectory("localization_panel");
        std::filesystem::create_directories(Workspace / "Assets");
        GameEngine::ApplicationConfig config{};
        config.Headless = true;
        config.EnableEditor = true;
        config.WorkspaceDirectory = Workspace.string();
        config.AssetDirectory = (Workspace / "Assets").string();
        auto& engine = GameEngine::EngineCore::GetInstance();
        ASSERT_TRUE(engine.Initialize(config));
        auto& assets = engine.GetAssetManager();
        for (const auto* alias : {"localization", "editor"})
        {
            GameEngine::AssetSourceDesc source{};
            source.Alias = alias;
            source.Root = StagedPackageAssets().parent_path() / alias;
            ASSERT_TRUE(assets.RegisterSource(source));
            assets.WaitForStartupScan(alias);
        }
    }
    static void TearDownTestSuite()
    {
        GameEngine::EngineCore::GetInstance().Shutdown();
        std::error_code error;
        std::filesystem::remove_all(Workspace, error);
    }
    void SetUp() override
    {
        Manager = std::make_unique<GameEngine::UIManager>(nullptr,
            &GameEngine::EngineCore::GetInstance().GetAssetManager());
        auto panel = std::make_unique<LocalizationPanel>();
        Panel = panel.get();
        Manager->SetRoot(std::move(panel));
        GameEngine::UILayoutAccess::SetLastLayoutRect(*Panel, 0, 0, 900, 700);
        static_cast<GameEngine::UIElement*>(Panel)->OnPostLayout();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            Deferred.Drain();
            Manager->GetDispatcher()->Drain();
            auto* status = dynamic_cast<GameEngine::Label*>(Panel->FindById("Status"));
            if (status && status->GetText().starts_with("0 keys")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL() << "the staged localization panel never finished binding";
    }
    void TearDown() override { Manager.reset(); Deferred.Drain(); }
    template<typename T> T* Find(const char* id) { return dynamic_cast<T*>(Panel->FindById(id)); }
    void Click(const char* id)
    {
        auto* button = Find<GameEngine::Button>(id);
        ASSERT_NE(button, nullptr) << id;
        button->TriggerClick();
        Deferred.Drain();
        Manager->GetDispatcher()->Drain();
    }
    void Draft(const std::string& key, const std::string& source)
    {
        Find<GameEngine::TextField>("TranslationKey")->SetValue(key);
        Find<GameEngine::TextField>("TranslationKey")->NotifyValueChanged();
        Find<GameEngine::TextArea>("Source")->SetValue(source);
        Find<GameEngine::TextArea>("Source")->NotifyValueChanged();
    }
    inline static std::filesystem::path Workspace;
    GameEngine::UI::UiDispatcher Deferred;
    GameEngine::UI::UiContextScope Scope{&Deferred, nullptr};
    std::unique_ptr<GameEngine::UIManager> Manager;
    LocalizationPanel* Panel = nullptr;
};
}

TEST_F(LocalizationPanelWorkflow, InvalidDraftIsRetainedWhenStartingAnotherKey)
{
    Draft("menu.play", "Play {name");
    EXPECT_FALSE(Find<GameEngine::Button>("Apply")->IsEnabled());
    Click("NewKey");
    EXPECT_EQ(Find<GameEngine::TextField>("TranslationKey")->GetValue(), "menu.play");
    EXPECT_EQ(Find<GameEngine::TextArea>("Source")->GetValue(), "Play {name");
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->HasClass("localization-status-error"));
    EXPECT_NE(Find<GameEngine::Label>("SourceError")->GetText().find("never closed"), std::string::npos);
    Click("Discard");
    EXPECT_TRUE(Find<GameEngine::TextField>("TranslationKey")->GetValue().empty());
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->GetText().starts_with("0 keys"));
}

TEST_F(LocalizationPanelWorkflow, StartingAnotherKeyCommitsAValidDraft)
{
    Draft("menu.play", "Play {name}");
    ASSERT_TRUE(Find<GameEngine::Button>("Apply")->IsEnabled());
    Click("NewKey");
    EXPECT_TRUE(Find<GameEngine::TextField>("TranslationKey")->GetValue().empty());
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->GetText().starts_with("1 key"));
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->GetText().ends_with(", unsaved."));
}

TEST_F(LocalizationPanelWorkflow, DiscardRestoresTheAppliedEntryWithoutDroppingIt)
{
    Draft("menu.play", "Play");
    Click("Apply");
    EXPECT_EQ(Find<GameEngine::Label>("KeyFixed")->GetText(), "menu.play");
    Find<GameEngine::TextArea>("Source")->SetValue("Broken {name");
    Find<GameEngine::TextArea>("Source")->NotifyValueChanged();
    EXPECT_FALSE(Find<GameEngine::Button>("Apply")->IsEnabled());
    Click("Discard");
    EXPECT_EQ(Find<GameEngine::TextArea>("Source")->GetValue(), "Play");
    EXPECT_FALSE(Find<GameEngine::Button>("Discard")->IsEnabled());
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->GetText().starts_with("1 key"));
}

TEST_F(LocalizationPanelWorkflow, AddingALanguagePreservesTheDraftItCommits)
{
    Draft("menu.play", "Play");
    Find<GameEngine::TextField>("NewLocale")->SetValue("nl");
    Click("AddLocale");
    EXPECT_TRUE(Find<GameEngine::Label>("Status")->GetText().starts_with("1 key"));
    EXPECT_EQ(Find<GameEngine::Label>("KeyFixed")->GetText(), "menu.play");
    EXPECT_EQ(Find<GameEngine::TextArea>("Source")->GetValue(), "Play");
    EXPECT_EQ(Find<GameEngine::Label>("Preview")->GetText(), "Play");
}

TEST(LocalizationEditorModule, CurrentKeyKeepsItsAccentWhileHovered)
{
    const auto css = ReadText(StagedPackageAssets() / LocalizationPanel::kStyleAssetPath);
    ASSERT_FALSE(css.empty());
    GameEngine::Stylesheet sheet;
    ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(
        css + "\n.localization-key-row { --ui_color_accent_translucent: #123456; "
              "--ui_color_bg_item_hover: #345678; }", sheet));
    GameEngine::UIElement row;
    row.AddClass("localization-key-row");
    GameEngine::UIParsing::ElementState state{};
    state.Hover = true;
    const auto hovered = GameEngine::UIParsing::CSSParser::ComputeStyleFor(row, sheet, state);
    EXPECT_EQ(hovered.Visual.BackgroundColor, 0xFF345678u);
    state.CustomStates.insert(GameEngine::HashStringId("current"));
    const auto current = GameEngine::UIParsing::CSSParser::ComputeStyleFor(row, sheet, state);
    EXPECT_EQ(current.Visual.BackgroundColor, 0xFF123456u);
}
