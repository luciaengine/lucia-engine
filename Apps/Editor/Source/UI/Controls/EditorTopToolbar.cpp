#include "UI/Controls/EditorTopToolbar.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "EditorPanelManager.h"
#include "Editor/Settings/AudioEnabledSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorPanelIds.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Input/InputSystem.h"
#include "Platform/ContextMenu.h"
#include "UI/EditorIcons.h"
#include "Platform/Window.h"
#include "PlayMode/PlayButtonMenu.h"
#include "PlayMode/PlayButtonState.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
struct ToolbarButtonInfo
{
    const char* id;
    const char* title;
    const char* iconPath;
    // The panel the button opens, or nullptr for an action. The right-click menu lists a
    // panel button among the panels, under the panel's title.
    const char* panelId = nullptr;
};

// The buttons declared in EditorTopToolbar.uxml (top-toolbar-left/center/right), with
// their titles for the right-click "show/hide icon" menu.
constexpr ToolbarButtonInfo kToolbarButtons[] = {
    {"TopToolbarUniversalSearch", "Universal Search",    "editor:Icons/usearch.svg"},
    {"TopToolbarLeft1",          "Save Scene",          "editor:Icons/save.svg"},
    {"TopToolbarLeft2",          "Open Project Folder", "editor:Icons/folder_open.svg"},
    {"TopToolbarPackages",       "Packages",            "editor:Icons/box.svg", EditorPanelIds::PackageManager},
    {"TopToolbarLeft3",          "Web Panel",           "editor:Icons/computer.svg", EditorPanelIds::Web},
    {"TopToolbarTodos",          "Todos",               "editor:Icons/todolist.svg", EditorPanelIds::Todo},
    {"TopToolbarBookmarks",      "Bookmarks",           "editor:Icons/book.svg", EditorPanelIds::Bookmarks},
    {"TopToolbarBuild",          "Build",               "editor:Icons/game-controller.svg", EditorPanelIds::Build},
    {"TopToolbarSettings",       "Settings",            "editor:Icons/settings.svg", EditorPanelIds::Settings},
    {"TopToolbarHelp",           "Help",                "editor:Icons/help.svg"},
    {"TopToolbarCenter2",        "Stop",                "editor:Icons/stop.svg"},
    {"TopToolbarRecord",         "Record",              "editor:Icons/recording_off_button.svg"},
    {"TopToolbarCenter3",        "Pause",               "editor:Icons/pause.svg"},
    {"TopToolbarCenter4",        "Play",                "editor:Icons/play.svg"},
    {"TopToolbarCenter1",        "Play Fullscreen",     "editor:Icons/fullscreen.svg"},
    {"TopToolbarUndoHistory",    "Undo History",        "editor:Icons/undo.svg", EditorPanelIds::UndoHistory},
    {"TopToolbarRight1",         "Script Editor",       "editor:Icons/log.svg", EditorPanelIds::ScriptEditor},
    {"TopToolbarNodeGraph",      "Node Graph",          "editor:Icons/node.svg"},
    {"TopToolbarMixer",          "Mixer",               "editor:Icons/mixer.svg", EditorPanelIds::Mixer},
    {"TopToolbarAnimation",      "Animation",           "editor:Icons/alarm.svg", EditorPanelIds::Animation},
    {"TopToolbarTimeline",       "Timeline",            "editor:Icons/film.svg", EditorPanelIds::Timeline},
    {"TopToolbarVisualProfiler", "Visual Profiler",     "editor:Icons/pulse.svg", EditorPanelIds::VisualProfiler},
    {"TopToolbarCpuProfiler",    "CPU Profiler",        "editor:Icons/timer.svg", EditorPanelIds::CpuProfiler},
    {"TopToolbarMonitors",       "Monitors",            "editor:Icons/stats.svg", EditorPanelIds::Monitors},
    {"TopToolbarVram",           "VRAM",                "editor:Icons/hardware-chip.svg", EditorPanelIds::Vram},
    {"TopToolbarRenderGraph",    "Render Graph",        "editor:Icons/pie-chart.svg", EditorPanelIds::RenderGraph},
    {"TopToolbarLog",            "Log",                 "editor:Icons/alert-circle.svg", EditorPanelIds::Log},
    {"TopToolbarTilePalette",    "Tile Palette",        "editor:Icons/TilePalette.svg", EditorPanelIds::TilePalette},
    {"TopToolbarRight2",         "Info",                "editor:Icons/info.svg"},
    {"TopToolbarPanelMenu",      "Panels",              "editor:Icons/menulist.svg"},
};

std::string MakeVisibilityPrefKey(const char* buttonId)
{
    std::string key = "ui.toolbar.hidden.";
    key += buttonId;
    return key;
}

bool CanHideToolbarButton(std::string_view buttonId)
{
    return buttonId != EditorPanelIds::TopToolbarPanelMenu;
}

const ToolbarButtonInfo* FindFixedPanelButton(std::string_view panelId)
{
    for (const ToolbarButtonInfo& info : kToolbarButtons)
        if (info.panelId && panelId == info.panelId)
            return &info;
    return nullptr;
}

std::string MakePanelButtonId(std::string_view panelId)
{
    std::string id = "TopToolbarPanel.";
    id += panelId;
    return id;
}

// Buttons live in the hugging icon container; the stretched section around it is the drop
// zone, so a drag can land in empty bar space.
const std::vector<Editor::ToolbarDragDrop::ContainerConfig>& TopToolbarContainers()
{
    static const std::vector<Editor::ToolbarDragDrop::ContainerConfig> kContainers = {
        {"toolbar-icons-left",   "ui.toolbar.left.v3.order",   "top-toolbar-left"},
        {"toolbar-icons-center", "ui.toolbar.center.v2.order", "top-toolbar-center"},
        {"toolbar-icons-right",  "ui.toolbar.right.v2.order",  "top-toolbar-right"},
    };
    return kContainers;
}

bool IsInSavedButtonOrder(const Editor::SettingsStore& prefs, const std::string& buttonId)
{
    for (const auto& container : TopToolbarContainers())
    {
        const auto it = prefs.Json().find(container.settingsKey);
        if (it == prefs.Json().end() || !it->is_array())
            continue;
        for (const auto& item : *it)
            if (item.is_string() && item.get<std::string>() == buttonId)
                return true;
    }
    return false;
}

// One row of the right-click menu: the actions first, then every panel of the Panels menu.
struct VisibilityRow
{
    std::string ButtonId;
    std::string Title;
    std::string IconPath;
    bool CanHide = true;
    bool SeparatorBefore = false;
};

std::vector<VisibilityRow> BuildVisibilityRows(const std::vector<EditorTopToolbar::PanelMenuEntry>& panels)
{
    std::vector<VisibilityRow> rows;
    rows.reserve(std::size(kToolbarButtons) + panels.size());
    for (const ToolbarButtonInfo& info : kToolbarButtons)
    {
        const bool listedAsPanel = info.panelId &&
            std::any_of(panels.begin(), panels.end(),
                        [&info](const EditorTopToolbar::PanelMenuEntry& p) { return p.panelId == info.panelId; });
        if (!listedAsPanel)
            rows.push_back({info.id, info.title, info.iconPath, CanHideToolbarButton(info.id), false});
    }
    for (const EditorTopToolbar::PanelMenuEntry& panel : panels)
    {
        const ToolbarButtonInfo* fixed = FindFixedPanelButton(panel.panelId);
        rows.push_back({fixed ? std::string(fixed->id) : MakePanelButtonId(panel.panelId), panel.title,
                        panel.iconPath, true, &panel == &panels.front()});
    }
    return rows;
}

// Element that owns a section's icon run, gaps included. Authored in
// EditorTopToolbar.uxml, sized by CSS to hug its buttons.
constexpr const char* kIconRunClass = "toolbar-icons";

void SetButtonHiddenInPrefs(const char* buttonId, bool hidden)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(MakeVisibilityPrefKey(buttonId), hidden);
    (void)prefs.Save(&err);
}

void ApplyHiddenClass(UIElement* el, bool hidden)
{
    if (!el)
        return;
    if (hidden)
    {
        if (!el->HasClass("hidden"))
            el->AddClass("hidden");
    }
    else
    {
        if (el->HasClass("hidden"))
            el->RemoveClass("hidden");
    }
    el->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

UIElement* FindFirstByClass(UIElement* el, const char* className)
{
    if (!el)
        return nullptr;
    if (el->HasClass(className))
        return el;
    for (const auto& child : el->GetChildren())
        if (auto* found = FindFirstByClass(child.get(), className))
            return found;
    return nullptr;
}

} // namespace

EditorTopToolbar::EditorTopToolbar()
{
    SetId("EditorTopToolbar");
    AddClass("editor-top-toolbar");
}

EditorTopToolbar::~EditorTopToolbar() = default;

bool EditorTopToolbar::LoadAssets()
{
    if (m_AssetsLoaded)
        return true;
    if (m_AssetLoadFailed || !GetOwnerManager())
        return false;

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID layoutGuid = assets.ResolveAssetGuid("UI/controls/EditorTopToolbar.uxml", "Editor");
    const GUID styleGuid = assets.ResolveAssetGuid("UI/controls/EditorTopToolbar.css", "Editor");
    if (layoutGuid.IsNull() || styleGuid.IsNull())
    {
        Logger::Log::Error("EditorTopToolbar: failed to resolve toolbar UI assets");
        m_AssetLoadFailed = true;
        return false;
    }

    // Waits on purpose: the editor's own .uxml and .css, small and never cooked, read once at startup.
    auto layoutAsset = assets.LoadAssetAsync(layoutGuid, AssetLoadPriority::High).get();
    auto styleAsset = assets.LoadAssetAsync(styleGuid, AssetLoadPriority::High).get();
    UIManager* ui = GetOwnerManager();
    if (!layoutAsset || layoutAsset->GetType() != AssetType::UILayout ||
        !styleAsset || styleAsset->GetType() != AssetType::UIStyle || !ui ||
        !ui->BindLayoutToSubtreeChildrenFromAsset(
            this, *static_cast<UILayoutAsset*>(layoutAsset.get())) ||
        !ui->AttachStyleToSubtreeFromAsset(
            this, *static_cast<UIStyleAsset*>(styleAsset.get())))
    {
        Logger::Log::Error("EditorTopToolbar: failed to load toolbar UI assets");
        m_AssetLoadFailed = true;
        return false;
    }

    m_AssetsLoaded = true;
    return true;
}

void EditorTopToolbar::OnPostLayout()
{
    if (!m_AssetsLoaded && !m_AssetLoadFailed)
        ScheduleAssetLoad();
}

void EditorTopToolbar::ScheduleAssetLoad()
{
    if (m_AssetLoadScheduled)
        return;
    m_AssetLoadScheduled = true;
    PostAction([this]()
    {
        m_AssetLoadScheduled = false;
        (void)LoadAssets();
    });
}

void EditorTopToolbar::WireControls(UIElement* rootEl, Platform::Window* window, const Callbacks& cb)
{
    if (!rootEl)
        return;

    // Universal Search launcher (same palette as Ctrl/Cmd+K).
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarUniversalSearch))))
    {
        btn->SetTooltip("Universal Search (Ctrl/Cmd+K)");
        if (cb.onUniversalSearchClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onUniversalSearchClicked](UIEvent&) { fn(); });
    }

    m_GetPanelMenuEntries = cb.getPanelMenuEntries;
    m_OpenPanel = cb.onPanelMenuEntryClicked;

    // Panel launcher. Its contents are queried on every click so panels
    // contributed by packages after startup are included automatically.
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarPanelMenu))))
    {
        btn->SetTooltip("Panels");
        if (window && cb.getPanelMenuEntries && cb.onPanelMenuEntryClicked)
        {
            btn->RegisterEventHandler(kEventButtonClick, [this, window, getEntries = cb.getPanelMenuEntries,
                             openPanel = cb.onPanelMenuEntryClicked](UIEvent& e)
            {
                UIElement& clicked = *e.CurrentTarget;
                auto entries = getEntries();
                std::sort(entries.begin(), entries.end(), [](const PanelMenuEntry& a, const PanelMenuEntry& b)
                {
                    if (a.title != b.title)
                        return a.title < b.title;
                    return a.panelId < b.panelId;
                });
                if (entries.empty())
                    return;

                m_PanelMenu = CreateContextMenu();
                if (!m_PanelMenu)
                    return;

                for (uint32_t i = 0; i < entries.size(); ++i)
                {
                    const uint32_t commandId = i + 1;
                    m_PanelMenu->AddItem(0, entries[i].title, commandId);
                    m_PanelMenu->SetItemIcon(commandId, entries[i].iconPath);
                }
                m_PanelMenu->SetCommandHandler(
                    [entries = std::move(entries), openPanel](uint32_t commandId)
                    {
                        if (commandId == 0 || commandId > entries.size())
                            return;
                        openPanel(entries[commandId - 1].panelId);
                    });

                const int x = static_cast<int>(clicked.GetLayoutX());
                const int y = static_cast<int>(clicked.GetLayoutY() + clicked.GetLayoutHeight());
                m_PanelMenu->Show(window, x, y);
            });
        }
    }

    // Settings button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarSettings))))
    {
        btn->SetTooltip("Settings");
        if (cb.onSettingsClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onSettingsClicked](UIEvent&) { fn(); });
    }

    // Help button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarHelp))))
    {
        btn->SetTooltip("Help");
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_PanelManager)
                m_PanelManager->ShowOrActivateHelpPanel();
        });
    }

    // Node Graph button
    if (auto* button = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarTilePalette))))
    {
        button->SetTooltip("Tile Palette");
        if (cb.onTilePaletteClicked)
            button->RegisterEventHandler(kEventButtonClick, [fn = cb.onTilePaletteClicked](UIEvent&) { fn(); });
    }

    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarNodeGraph))))
    {
        btn->SetTooltip("Node Graph");
        if (cb.onNodeGraphClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onNodeGraphClicked](UIEvent&) { fn(); });
    }

    // Bookmarks button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarBookmarks))))
    {
        btn->SetTooltip("Bookmarks");
        if (cb.onBookmarksClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onBookmarksClicked](UIEvent&) { fn(); });
    }

    // Fullscreen toggle button (play fullscreen preference)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById("TopToolbarCenter1"_sid)))
    {
        btn->SetTooltip("Play Fullscreen");
        m_FullscreenButton = btn;
        if (cb.onFullscreenToggleClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onFullscreenToggleClicked](UIEvent&) { fn(); });
    }

    // Stop button
    if (auto* stopBtn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarCenter2))))
    {
        stopBtn->SetTooltip("Stop");
        m_StopButton = stopBtn;
        if (cb.onStopClicked)
            stopBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onStopClicked](UIEvent&) { fn(); });
    }

    // Movie record shortcut
    if (auto* recordBtn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarRecord))))
    {
        recordBtn->SetTooltip("Record");
        m_RecordButton = recordBtn;
        if (cb.onRecordClicked)
            recordBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onRecordClicked](UIEvent&) { fn(); });
    }

    // Pause button
    if (auto* pauseBtn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarCenter3))))
    {
        pauseBtn->SetTooltip("Pause");
        m_PauseButton = pauseBtn;
        if (cb.onPauseClicked)
            pauseBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPauseClicked](UIEvent&) { fn(); });
    }

    // Play button. Its right-click menu carries the editor-wide "Audio Enabled" toggle; the
    // saved choice is applied to the engine's audio system here, at startup.
    if (auto* playBtn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarCenter4))))
    {
        m_PlayButton = playBtn;
        if (cb.onPlayClicked)
            playBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPlayClicked](UIEvent&) { fn(); });
        Audio::AudioSystem* audio = EngineCore::GetInstance().GetAudioSystem();
        SetAudioEnabled(Editor::AudioEnabledSettings::ApplySaved(audio));
        playBtn->AddManipulator(ContextMenuManipulator::Create(
            [this, audio]
            {
                return Editor::PlayButtonMenuItems(audio,
                    [this](bool enabled) { SetAudioEnabled(enabled); });
            }));
    }

    // Save scene (same as Editor.Global.SaveScene: save to path, or Save As modal if untitled)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarLeft1))))
    {
        btn->SetTooltip("Save Scene");
        m_SaveButton = btn;
        if (cb.onSaveSceneClicked)
        {
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onSaveSceneClicked](UIEvent& e) { fn(e.Mods); });
            std::vector<ContextMenuManipulator::Item> saveItems = {
                {.Path = "Save",
                 .IconPath = EditorIcons::kSave,
                 .OnActivate = [fn = cb.onSaveSceneClicked] { fn(0); }},
                {.Path = "Save As...",
                 .IconPath = EditorIcons::kSave,
                 .OnActivate = [fn = cb.onSaveSceneClicked] { fn(Input::kModShift); }},
            };
            if (cb.onRevertSceneClicked)
            {
                saveItems.push_back({.Separator = true});
                saveItems.push_back({
                    .Path = "Revert Scene",
                    .IconPath = EditorIcons::kReset,
                    .OnActivate = [fn = cb.onRevertSceneClicked] { fn(); },
                });
            }
            btn->AddManipulator(ContextMenuManipulator::Create(std::move(saveItems)));
            btn->SetTooltip("Save Scene. Right-click for options.");
        }
    }

    // Project folder button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarLeft2))))
    {
        btn->SetTooltip("Open Project Folder");
        if (cb.onProjectFolderClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onProjectFolderClicked](UIEvent&) { fn(); });
    }

    // Web panel button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarLeft3))))
    {
        btn->SetTooltip("Web Panel");
        if (cb.onWebClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onWebClicked](UIEvent&) { fn(); });
    }

    // Script Editor button (with legacy id fallback)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarRight1))))
    {
        btn->SetTooltip("Script Editor");
        if (cb.onScriptEditorClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onScriptEditorClicked](UIEvent&) { fn(); });
    }
    else if (auto* legacyBtn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarScriptEditor))))
    {
        legacyBtn->SetTooltip("Script Editor");
        if (cb.onScriptEditorClicked)
            legacyBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onScriptEditorClicked](UIEvent&) { fn(); });
    }

    // Todos button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarTodos))))
    {
        btn->SetTooltip("Todos");
        if (cb.onTodosClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onTodosClicked](UIEvent&) { fn(); });
    }

    // Undo History button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarUndoHistory))))
    {
        btn->SetTooltip("Undo History");
        if (cb.onUndoHistoryClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onUndoHistoryClicked](UIEvent&) { fn(); });
    }

    // Packages button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarPackages))))
    {
        btn->SetTooltip("Packages");
        if (cb.onPackagesClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPackagesClicked](UIEvent&) { fn(); });
    }

    // Build button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarBuild))))
    {
        btn->SetTooltip("Build");
        if (cb.onBuildClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onBuildClicked](UIEvent&) { fn(); });
    }

    // Mixer button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarMixer))))
    {
        btn->SetTooltip("Mixer");
        if (cb.onMixerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onMixerClicked](UIEvent&) { fn(); });
    }

    // Visual Profiler button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarVisualProfiler))))
    {
        btn->SetTooltip("Visual Profiler");
        if (cb.onVisualProfilerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onVisualProfilerClicked](UIEvent&) { fn(); });
    }

    // Monitors button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarMonitors))))
    {
        btn->SetTooltip("Monitors");
        if (cb.onMonitorsClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onMonitorsClicked](UIEvent&) { fn(); });
    }

    // Render Graph button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarRenderGraph))))
    {
        btn->SetTooltip("Render Graph");
        if (cb.onRenderGraphClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onRenderGraphClicked](UIEvent&) { fn(); });
    }

    // Animation button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarAnimation))))
    {
        btn->SetTooltip("Animation");
        if (cb.onAnimationClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onAnimationClicked](UIEvent&) { fn(); });
    }

    // Timeline button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarTimeline))))
    {
        btn->SetTooltip("Timeline");
        if (cb.onTimelineClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onTimelineClicked](UIEvent&) { fn(); });
    }

    // Log button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarLog))))
    {
        btn->SetTooltip("Log");
        if (cb.onLogClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onLogClicked](UIEvent&) { fn(); });
    }

    // VRAM button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarVram))))
    {
        btn->SetTooltip("VRAM");
        if (cb.onVramClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onVramClicked](UIEvent&) { fn(); });
    }

    // CPU Profiler button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarCpuProfiler))))
    {
        btn->SetTooltip("CPU Profiler");
        if (cb.onCpuProfilerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onCpuProfilerClicked](UIEvent&) { fn(); });
    }
}

void EditorTopToolbar::UpdatePlayModeState(Editor::PlayModeState s)
{
    // Toolbar behavior:
    // - Play: see Editor::ApplyPlayButtonState (clickable in Edit, menu reachable during
    //         play, the green icon from its own state class while play mode runs).
    // - Pause: enabled only while playing/paused; pressed styling indicates paused.
    // - Stop: enabled while playing/paused/review; disabled in Edit.
    if (m_PlayButton)
        Editor::ApplyPlayButtonState(*m_PlayButton, s);
    if (m_PauseButton)
    {
        const bool enablePause =
            (s == Editor::PlayModeState::Play || s == Editor::PlayModeState::Paused);
        m_PauseButton->SetEnabled(enablePause);
        if (s == Editor::PlayModeState::Paused)
            m_PauseButton->AddClass("pressed");
        else
            m_PauseButton->RemoveClass("pressed");
    }
    if (m_StopButton)
    {
        const bool enableStop =
            (s == Editor::PlayModeState::Play ||
             s == Editor::PlayModeState::Paused ||
             s == Editor::PlayModeState::ChangeReview ||
             s == Editor::PlayModeState::EnteringPlay ||
             s == Editor::PlayModeState::ExitingPlay);
        m_StopButton->SetEnabled(enableStop);
    }
}

void EditorTopToolbar::SetSaveDirty(bool dirty)
{
    if (!m_SaveButton)
        return;

    if (dirty)
        m_SaveButton->AddClass("save-dirty-active");
    else
        m_SaveButton->RemoveClass("save-dirty-active");
    m_SaveButton->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);

    for (const auto& child : m_SaveButton->GetChildren())
    {
        if (child && child->HasClass("save-dirty-indicator"))
        {
            if (dirty)
                child->RemoveClass("hidden");
            else
                child->AddClass("hidden");
            child->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            break;
        }
    }
}

void EditorTopToolbar::SetFullscreenActive(bool active)
{
    if (!m_FullscreenButton)
        return;
    if (active)
        m_FullscreenButton->AddClass("pressed");
    else
        m_FullscreenButton->RemoveClass("pressed");
}

void EditorTopToolbar::SetAudioEnabled(bool enabled)
{
    if (m_PlayButton)
        m_PlayButton->SetTooltip(enabled ? "Play. Right-click for options." : "Play (audio muted). Right-click for options.");
}

void EditorTopToolbar::SetRecordActive(bool active)
{
    if (!m_RecordButton)
        return;
    if (active)
        m_RecordButton->AddClass("active");
    else
        m_RecordButton->RemoveClass("active");
}

void EditorTopToolbar::SetupDragDrop(UIElement* rootEl, Editor::ToolbarDragDrop* dragDrop)
{
    if (!dragDrop || !rootEl)
        return;

    m_DragDrop = dragDrop;
    dragDrop->Setup(rootEl, TopToolbarContainers());
    dragDrop->LoadButtonOrder(rootEl, TopToolbarContainers());
}

void EditorTopToolbar::SyncPanelButtons(UIElement* rootEl)
{
    if (!rootEl || !m_GetPanelMenuEntries || !m_OpenPanel)
        return;
    UIElement* search = rootEl->FindById(HashStringId(EditorPanelIds::TopToolbarUniversalSearch));
    if (!search || !search->GetParent())
        return;

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    bool added = false;
    bool addedSavedButton = false;
    for (const PanelMenuEntry& panel : m_GetPanelMenuEntries())
    {
        const std::string buttonId = MakePanelButtonId(panel.panelId);
        if (FindFixedPanelButton(panel.panelId) || rootEl->FindById(HashStringId(buttonId)))
            continue;
        AddPanelButton(*search, panel, buttonId, prefs);
        added = true;
        addedSavedButton = addedSavedButton || IsInSavedButtonOrder(prefs, buttonId);
    }
    if (!added || !m_DragDrop)
        return;
    m_DragDrop->Setup(rootEl, TopToolbarContainers());
    // A button the user moved before goes back to its saved place; a new one stays before search.
    if (addedSavedButton)
        m_DragDrop->LoadButtonOrder(rootEl, TopToolbarContainers());
}

void EditorTopToolbar::AddPanelButton(UIElement& before, const PanelMenuEntry& panel,
                                      const std::string& buttonId, const Editor::SettingsStore& prefs)
{
    UIElement* container = before.GetParent();
    size_t index = 0;
    while (index < container->GetChildren().size() && container->GetChildren()[index].get() != &before)
        ++index;

    auto button = std::make_unique<Button>();
    button->SetId(buttonId);
    for (const char* cls : {"small", "secondary", "icon-button", "toolbar-outline-icon", "toolbar-panel-button"})
        button->AddClass(cls);
    button->AddClass(panel.iconClass.empty() ? "menulist-icon" : panel.iconClass.c_str());
    button->SetTooltip(panel.title);
    button->RegisterEventHandler(kEventButtonClick,
                                 [open = m_OpenPanel, panelId = panel.panelId](UIEvent&) { open(panelId); });
    bool hidden = !panel.showOnTopToolbarByDefault;
    prefs.TryGetBool(MakeVisibilityPrefKey(buttonId.c_str()), hidden);
    UIElement* raw = button.get();
    container->InsertChild(index, std::move(button));
    ApplyHiddenClass(raw, hidden);
}

void EditorTopToolbar::ApplyButtonVisibilityFromPrefs(UIElement* rootEl)
{
    if (!rootEl)
        return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    for (const auto& info : kToolbarButtons)
    {
        bool hidden = false;
        if (CanHideToolbarButton(info.id))
            prefs.TryGetBool(MakeVisibilityPrefKey(info.id), hidden);
        ApplyHiddenClass(rootEl->FindById(HashStringId(info.id)), hidden);
    }
}

void EditorTopToolbar::WireRightClickToggles(UIElement* rootEl, Platform::Window* window)
{
    if (!rootEl || !window)
        return;

    auto showVisibilityMenu = [this, rootEl, window](int x, int y)
    {
        m_ButtonVisibilityMenu = CreateContextMenu();
        if (!m_ButtonVisibilityMenu)
            return;

        std::vector<VisibilityRow> rows =
            BuildVisibilityRows(m_GetPanelMenuEntries ? m_GetPanelMenuEntries() : std::vector<PanelMenuEntry>{});
        for (uint32_t i = 0; i < rows.size(); ++i)
        {
            const VisibilityRow& row = rows[i];
            UIElement* button = rootEl->FindById(HashStringId(row.ButtonId));
            const bool visible = button && !button->HasClass("hidden");
            const uint32_t commandId = i + 1;
            uint32_t flags = visible ? MenuItemFlag_Checked : MenuItemFlag_None;
            if (!row.CanHide || !button)
                flags |= MenuItemFlag_Disabled;
            if (row.SeparatorBefore)
                m_ButtonVisibilityMenu->AddSeparator(0);
            m_ButtonVisibilityMenu->AddItem(0, row.Title, commandId, flags);
            m_ButtonVisibilityMenu->SetItemIcon(commandId, row.IconPath);
        }

        m_ButtonVisibilityMenu->SetCommandHandler([rootEl, rows = std::move(rows)](uint32_t commandId)
        {
            if (commandId == 0 || commandId > rows.size())
                return;
            const VisibilityRow& row = rows[commandId - 1];
            if (!row.CanHide)
                return;
            UIElement* button = rootEl->FindById(HashStringId(row.ButtonId));
            if (!button)
                return;
            const bool nowHidden = !button->HasClass("hidden");
            SetButtonHiddenInPrefs(row.ButtonId.c_str(), nowHidden);
            ApplyHiddenClass(button, nowHidden);
        });

        m_ButtonVisibilityMenu->Show(window, x, y);
    };

    // The customization menu belongs to the empty runs of the toolbar. Save and Record
    // consume their own right-clicks through the ContextMenuManipulator attached to each;
    // Button itself claims no right-click, so every other button leaves one unhandled and it
    // bubbles here. The original hit target settles both cases: an event that came through a
    // Button or through the icon container came from the icons, inter-icon gaps included.
    //
    // This handler is the manipulator's own second consumer, and it is not one yet: it
    // activates on a bare right RELEASE, so a press that starts on a button and ends in a gap
    // opens this menu — exactly the unarmed-gesture defect PointerManipulator exists to stop.
    // Migrating it needs one piece the manipulator does not have, a press-time gate, because
    // the "did this come from a button?" test reads the press target and arming replaces the
    // release target with the capture element. Tracked, not done here.
    auto* toolbar = FindFirstByClass(rootEl, "editor-top-toolbar");
    if (!toolbar)
        return;

    toolbar->RegisterEventHandler(kEventMouseUp, [toolbar, showVisibilityMenu](UIEvent& e)
    {
        if (e.Button != 1)
            return;

        for (UIElement* node = e.Target; node && node != toolbar; node = node->GetParent())
        {
            if (dynamic_cast<Button*>(node) || node->HasClass(kIconRunClass))
                return;
        }

        showVisibilityMenu(static_cast<int>(e.X), static_cast<int>(e.Y));
        e.Stop();
    });
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_editorTopToolbar =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorTopToolbar>(
        "EditorTopToolbar",
        []() { return std::make_unique<GameEngine::EditorTopToolbar>(); });
}
