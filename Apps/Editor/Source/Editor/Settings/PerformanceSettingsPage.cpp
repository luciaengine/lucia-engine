#include "Editor/Settings/PerformanceSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// Read at startup, before the device exists, to pick the backend.
constexpr const char* kPrefKeyGraphicsApi = "renderer.graphicsApi";
constexpr const char* kGraphicsApiAuto = "auto";
constexpr const char* kPrefKeyVsync = "performance.vsync";
constexpr bool kDefaultVsync = true;
constexpr float kRenderScaleStep = 0.05f;

Rendering::IDevice* RunningDevice()
{
    if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        return renderServices->GetDevice();
    return nullptr;
}

std::vector<SettingsFieldDescriptor::DropdownField::Option> GraphicsApiOptions()
{
    std::vector<SettingsFieldDescriptor::DropdownField::Option> options = {{kGraphicsApiAuto, "Auto"}};
    if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::Vulkan))
        options.push_back({"vulkan", "Vulkan"});
    if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::Metal))
        options.push_back({"metal", "Metal"});
    if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::DirectX12))
        options.push_back({"dx12", "DirectX 12"});
    return options;
}

std::string RunningGraphicsApiCard()
{
    const char* activeApi = "unknown";
    if (Rendering::IDevice* device = RunningDevice())
    {
        switch (device->GetAPI())
        {
            case Rendering::GraphicsAPI::Vulkan: activeApi = "Vulkan"; break;
            case Rendering::GraphicsAPI::Metal: activeApi = "Metal"; break;
            case Rendering::GraphicsAPI::DirectX12: activeApi = "DirectX 12"; break;
            case Rendering::GraphicsAPI::WebGPU: activeApi = "WebGPU"; break;
            case Rendering::GraphicsAPI::Auto: activeApi = "Auto"; break;
        }
    }
    return std::string("Currently running: ") + activeApi +
           ". Graphics backend used for rendering; takes effect after restarting the editor. "
           "The GE_GFX_API environment variable overrides this setting.";
}

// The device's own state wins over the saved preference: startup config, the
// GE_VSYNC override and runtime toggles can all make them differ, and the row
// must show what the editor is doing.
bool RunningVsync()
{
    if (Rendering::IDevice* device = RunningDevice())
        return device->IsVsyncEnabled();
    auto prefs = OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    bool stored = kDefaultVsync;
    prefs.TryGetBool(kPrefKeyVsync, stored);
    return stored;
}
} // namespace

void RegisterPerformanceSettingsCategory(std::function<void(bool)> applyVsync)
{
    SettingsCategoryDescriptor performance;
    performance.CategoryId = "performance";
    performance.Title = "Performance";
    performance.Group = SettingsCategoryGroup::UserSettings;
    performance.TreeRowClass = "performance-row";
    performance.SearchKeywords = "performance frame rate fps gpu";

    {
        SettingsFieldDescriptor api;
        api.Label = "Graphics API";
        api.SearchKeywords = "graphics api backend renderer vulkan metal directx dx12";
        api.PrefKey = kPrefKeyGraphicsApi;
        SettingsFieldDescriptor::DropdownField dropdown;
        dropdown.OptionsProvider = GraphicsApiOptions;
        dropdown.DefaultValue = kGraphicsApiAuto;
        api.Control = std::move(dropdown);
        performance.Fields.push_back(std::move(api));
    }

    {
        SettingsFieldDescriptor vsync;
        vsync.Label = "VSync";
        vsync.SearchKeywords = "vsync vertical sync frame rate display refresh";
        vsync.PrefKey = kPrefKeyVsync;
        vsync.InfoCard = "Synchronize rendering to the display refresh rate. Limits the editor to your "
                         "monitor's refresh rate but eliminates screen tearing.";
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = kDefaultVsync;
        toggle.Get = RunningVsync;
        toggle.Set = std::move(applyVsync);
        vsync.Control = std::move(toggle);
        performance.Fields.push_back(std::move(vsync));
    }

    {
        SettingsFieldDescriptor renderScale;
        renderScale.Label = "Scene View Render Scale";
        renderScale.SearchKeywords = "scene view render scale resolution supersampling ssaa gpu";
        renderScale.InfoCard =
            "Editor-side override of the project Render Scale for the Scene and Game views. "
            "1.0 = no override, and the views follow the project setting. Lower values trade "
            "image sharpness for GPU performance (most noticeable on macOS at 2x Retina); above "
            "1.0 supersamples (SSAA). Clustered lighting and depth-min/max compute follow the "
            "scaled extent so lighting stays correct.";
        // SceneViewSettings persists sceneView.renderScale itself. The view
        // controllers republish the override every frame, so the drag previews
        // without saving and the release saves.
        SettingsFieldDescriptor::SliderField slider;
        slider.DefaultValue = SceneViewSettings::kDefaultRenderScale;
        slider.MinValue = SceneViewSettings::kMinRenderScale;
        slider.MaxValue = SceneViewSettings::kMaxRenderScale;
        slider.Step = kRenderScaleStep;
        slider.Get = []() { return SceneViewSettings::Get().GetRenderScale(); };
        slider.Preview = [](float value) { SceneViewSettings::Get().SetRenderScale(value, false); };
        slider.Set = [](float value) { SceneViewSettings::Get().SetRenderScale(value); };
        renderScale.Control = std::move(slider);
        performance.Fields.push_back(std::move(renderScale));
    }

    // Which backend is running is only known once the device exists.
    performance.PrepareFields = [](SettingsCategoryDescriptor& page)
    { page.Fields.front().InfoCard = RunningGraphicsApiCard(); };

    EditorSettingsRegistry::Get().RegisterCategory(std::move(performance));
}

} // namespace GameEngine::Editor
