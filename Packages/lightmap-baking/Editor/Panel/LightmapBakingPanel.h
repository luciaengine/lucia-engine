#pragma once

#include "UI/Controls/DockPanel.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Toggle;
class Dropdown;
class FloatField;
class IntField;
class Label;
class TextField;
namespace Editor { class LightmapBakeService; }

class LightmapBakingPanel : public DockPanel
{
public:
    LightmapBakingPanel();
    ~LightmapBakingPanel() override;
    std::function<void()> OnDestroyed;
    void SetServices(std::shared_ptr<Editor::LightmapBakeService> service,
                     std::function<std::vector<uint32_t>()> selection,
                     std::function<bool()> canBake);
    void Update();

private:
    void AddSelection(bool receivers);
    void RefreshMeshLabels();
    void StartBake();
    void ShowMessage(const std::string& text, bool error = false);
    std::vector<uint32_t> SceneOccluders() const;
    std::string MeshNames(const std::vector<uint32_t>& ids) const;

    std::shared_ptr<Editor::LightmapBakeService> m_Service;
    std::function<std::vector<uint32_t>()> m_GetSelection;
    std::function<bool()> m_CanBake;
    std::vector<uint32_t> m_Receivers, m_Occluders;
    uint64_t m_SceneVersion = 0;
    bool m_ControlsEnabled = true;
    std::string m_ProjectRoot, m_LastResult, m_ActiveJob;
    std::chrono::steady_clock::time_point m_NextRefresh{};
    UIElement* m_Settings = nullptr;
    UIElement* m_CustomOccluders = nullptr;
    Label* m_ReceiverLabel = nullptr;
    Label* m_OccluderLabel = nullptr;
    Label* m_LightLabel = nullptr;
    Label* m_Status = nullptr;
    Label* m_ProgressLabel = nullptr;
    Dropdown* m_Resolution = nullptr;
    Dropdown* m_ShadowMode = nullptr;
    Toggle* m_AllOccluders = nullptr;
    Toggle* m_GI = nullptr;
    Toggle* m_Denoise = nullptr;
    IntField* m_Samples = nullptr;
    IntField* m_Bounces = nullptr;
    IntField* m_ShadowSamples = nullptr;
    FloatField* m_FadeStart = nullptr;
    FloatField* m_FadeEnd = nullptr;
    FloatField* m_Ambient = nullptr;
    TextField* m_Environment = nullptr;
    FloatField* m_EnvironmentIntensity = nullptr;
    Button* m_Bake = nullptr;
    Button* m_Cancel = nullptr;
};
}
