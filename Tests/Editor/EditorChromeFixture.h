#pragma once

#include "Panels/LogPanel.h"
#include "Panels/LogView.h"
#include "Core/ApplicationPaths.h"
#include "Assets/AssetManager.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Shell.h"
#include "TestTempDir.h"
#include "FixedScalePlatform.h"
#include "UIRgTestHarness.h"
#include "UIPixelReadback.h"
#include "UI/Controls/Button.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/EditorSearchBars.h"
#include "UI/StyleProperties.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>

using namespace GameEngine;
namespace EditorChromeTesting
{
inline UIElement* ClassIn(UIElement* root, const char* name)
{
    if (root->HasClass(name)) return root;
    for (const auto& child : root->GetChildren())
        if (auto* found = ClassIn(child.get(), name)) return found;
    return nullptr;
}
inline void Click(UIElement* element)
{
    UIEvent event{};
    event.Id = kEventButtonClick;
    event.Target = element;
    event.CurrentTarget = element;
    element->DispatchEvent(event);
}
struct EditorChromeHost
{
    std::unique_ptr<Rendering::IDevice> Device = MakeHeadlessDevice();
    TestUtils::ScopedTempDir Temp{TestUtils::MakeUniqueTempDirectory("log-panel-assets")};
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    UITesting::FixedScalePlatform Platform{1.0f};
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Render;
    LogPanel* Panel = nullptr;
    bool Ready = false;
    bool PreviousSearchVisible = false;
    bool SearchVisibilityChanged = false;

    explicit EditorChromeHost(bool showLogPanel = true)
    {
        if (!Device) return;
        UIRegistration::RegisterBuiltInControls();
        const auto staged = ApplicationPaths::GetInstallAssetsRoot();
        for (const char* icon : {"copy.svg", "trash.svg", "checked.svg", "Log/pause.svg", "Log/resume.svg"})
            if (!std::filesystem::exists(staged / "Icons" / icon)) return;
        AssetSourceDesc source;
        source.Alias = "editor";
        source.Root = staged;
        if (!Assets.Initialize(Temp.Path() / "Assets", &Pool, Temp.Path() / "assets.assetdb", Temp.Path() / "Cache") ||
            !Assets.RegisterSource(source)) return;
        Assets.WaitForStartupScan("editor");
        Ui = std::make_unique<UIManager>(Device.get(), &Assets);
        Ui->SetPlatform(&Platform);
        Ui->SetLayoutSizeOverride(800, 600);
        Ui->SetTextBlendGamma(1.0f);
        for (const char* name : {"theme/tokens.css", "theme/core.css", "controls/Button.css", "theme/widgets.css", "theme/views.css"})
        {
            std::ifstream input(staged / "UI" / name);
            if (!input) return;
            std::ostringstream text; text << input.rdbuf();
            auto sheet = std::make_shared<Stylesheet>();
            if (!UIParsing::CSSParser::ParseStylesFromString(text.str(), *sheet)) return;
            sheet->SourceName = (staged / "UI" / name).string();
            Ui->AddStylesheet(sheet);
        }
        auto pose = std::make_shared<Stylesheet>();
        if (!UIParsing::CSSParser::ParseStylesFromString(
            "#LogTestRoot { width: 800px; height: 600px; display: flex; flex-direction: column; }"
            "#LogTestPanel { width: 600px; height: 540px; display: flex; flex-direction: column; }", *pose)) return;
        Ui->AddStylesheet(pose);
        auto root = std::make_unique<UIElement>(); root->SetId("LogTestRoot");
        if (showLogPanel)
        {
            PreviousSearchVisible = EditorSearchBars::GetVisible();
            EditorSearchBars::SetVisible(true);
            SearchVisibilityChanged = true;
            auto panel = std::make_unique<LogPanel>(); Panel = panel.get(); panel->SetId("LogTestPanel");
            root->AddChild(std::move(panel));
        }
        Ui->SetRoot(std::move(root));
        Render = std::make_unique<UiRgHarness>(Device.get());
        Ready = true;
        Settle();
    }
    ~EditorChromeHost()
    {
        Render.reset();
        Ui.reset(); // Unregister the panel's raw search pointer before restoring the setting.
        if (SearchVisibilityChanged) EditorSearchBars::SetVisible(PreviousSearchVisible);
    }
    void Settle()
    {
        for (int frame = 0; frame < 20; ++frame)
        {
            Ui->Update(0.016f, true);
            if (Panel) Panel->Update();
            DriveUiRender(*Ui, *Render);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    Button* Action(const char* id) { return dynamic_cast<Button*>(Panel->FindById(HashStringId(id))); }
    LogView* View() { return dynamic_cast<LogView*>(Panel->FindById("LogView"_sid)); }
};
}

