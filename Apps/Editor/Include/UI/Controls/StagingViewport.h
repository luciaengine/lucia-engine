#pragma once

#include "Editor/RenderGraphTickRegistry.h"
#include "Engine/Rendering/StagingView.h"
#include "UI/UIElement.h"

namespace GameEngine::Engine::Renderer
{
class StagingViewPool;
struct StagingViewDesc;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Editor
{
/**
 * @brief Shows a pooled staging view in a panel: the control's image shows the view's output at the control's
 * size in device pixels. The control owns the view; the host renders it (the Asset View's thumbnail lane).
 * Each instance has its own view and texture. Main thread only.
 */
class StagingViewport final : public UIElement
{
  public:
    StagingViewport(Engine::Renderer::StagingViewPool& pool, RenderGraphTickRegistry& ticks,
                    const Engine::Renderer::StagingViewDesc& desc);
    ~StagingViewport() override;
    StagingViewport(const StagingViewport&) = delete;
    StagingViewport& operator=(const StagingViewport&) = delete;

    /// The view the control shows. The host spawns into its world, sets its camera and declares its passes.
    /// It is not alive while the host is hidden, and after renderer teardown released it.
    Engine::Renderer::StagingView& View() { return m_View; }
    /// The host calls it when its panel shows or hides. A hidden control releases its view to the pool, so it
    /// holds no world, camera or output; a shown control acquires a new view, and its output stays hidden until the host shows it.
    void SetHostVisible(bool visible);
    /// Shows or hides the output. The control keeps its layout size, so the view keeps rendering at that size.
    void SetOutputShown(bool shown);
    /// Shows the output once the view writes its next frame. Until then the output keeps its state: a shown
    /// output keeps the last frame on screen, a hidden one stays hidden.
    void ShowAfterNextFrame();
    bool IsOutputShown() const { return m_OutputShown; }

  protected:
    /// The view renders at this element's laid-out size in device pixels.
    void OnPostLayout() override;

  private:
    void BuildLayout();
    void TickFrame(uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame);
    void PublishOutput(UIManager& ui, Rendering::RenderGraph::RGFrame& frame);
    void AcquireView();
    void ReleaseView();
    void ForgetPool();
    void ApplyOutputShown(bool shown);

    Engine::Renderer::StagingViewPool* m_Pool = nullptr;
    RenderGraphTickRegistry& m_Ticks;
    RenderGraphTickRegistry::Handle m_Tick = RenderGraphTickRegistry::kInvalidHandle;
    Engine::Renderer::StagingView m_View;
    UIElement* m_Image = nullptr;
    std::string m_DebugName;
    std::string m_TextureName;
    uint32 m_Width = 0;
    uint32 m_Height = 0;
    bool m_OutputShown = true;
    bool m_ShowAfterNextFrame = false;
    uint64 m_ShowAfterRevision = 0;
};
} // namespace GameEngine::Editor
