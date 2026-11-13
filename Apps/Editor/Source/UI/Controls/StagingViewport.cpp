#include "UI/Controls/StagingViewport.h"

#include "UI/ControlLayoutLoader.h"

#include "Engine/Rendering/StagingViewPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <array>
#include <cmath>
#include <string_view>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/StagingViewport/StagingViewport.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/StagingViewport/StagingViewport.css";
constexpr std::array<std::string_view, 1> kRequiredIds = {"staging-viewport-image"};
} // namespace

StagingViewport::StagingViewport(Engine::Renderer::StagingViewPool& pool, RenderGraphTickRegistry& ticks,
                                 const Engine::Renderer::StagingViewDesc& desc)
    : m_Pool(&pool)
    , m_Ticks(ticks)
    , m_DebugName(desc.DebugName ? desc.DebugName : "Staging View")
    , m_Width(desc.Width)
    , m_Height(desc.Height)
{
    BuildLayout();
    AcquireView();
}

StagingViewport::~StagingViewport()
{
    ReleaseView();
}

void StagingViewport::SetHostVisible(bool visible)
{
    if (!visible)
    {
        ReleaseView();
        ApplyOutputShown(false);
        return;
    }
    if (!m_Pool || m_View.IsAlive())
        return;
    AcquireView();
}

void StagingViewport::SetOutputShown(bool shown)
{
    m_ShowAfterNextFrame = false;
    ApplyOutputShown(shown);
}

void StagingViewport::ShowAfterNextFrame()
{
    m_ShowAfterNextFrame = true;
    m_ShowAfterRevision = m_View.IsAlive() ? m_View.OutputRevision() : 0;
}

void StagingViewport::ApplyOutputShown(bool shown)
{
    m_OutputShown = shown;
    // Only the image hides; the view follows this element's box, which the host sizes.
    if (m_Image)
        m_Image->Style().Set(Style::Display, shown ? DisplayMode::Flex : DisplayMode::None);
}

void StagingViewport::BuildLayout()
{
    AddClass("staging-viewport");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> layout = ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!layout)
        return;
    m_Image = layout->FindById("staging-viewport-image"_sid);
    AddChild(std::move(layout));
}

void StagingViewport::AcquireView()
{
    m_View = m_Pool->Acquire(Engine::Renderer::StagingViewDesc{m_Width, m_Height, m_DebugName.c_str()});
    m_TextureName = m_View.UiTextureName();
    m_Tick = m_Ticks.Register([this](uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame)
                              { TickFrame(windowId, ui, frame); },
                              [this] { ForgetPool(); });
    if (m_Image)
        UI::Layout::SetBackgroundResourceName(*m_Image, m_TextureName);
}

void StagingViewport::TickFrame(uint64_t, UIManager* ui, Rendering::RenderGraph::RGFrame& frame)
{
    if (!ui || ui != GetOwnerManager() || !m_View.IsAlive())
        return;
    PublishOutput(*ui, frame);
    if (m_ShowAfterNextFrame && m_View.OutputRevision() > m_ShowAfterRevision)
        SetOutputShown(true);
}

void StagingViewport::OnPostLayout()
{
    UIElement::OnPostLayout();
    const UIManager* ui = GetOwnerManager();
    const float scale = ui ? ui->GetContentScale() : 1.0f;
    const uint32 width = static_cast<uint32>(std::lround(GetLayoutWidth() * scale));
    const uint32 height = static_cast<uint32>(std::lround(GetLayoutHeight() * scale));
    // An empty rect (a collapsed panel) keeps the last size.
    if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
        return;
    m_Width = width;
    m_Height = height;
    if (m_View.IsAlive())
        m_Pool->Resize(m_View, width, height);
}

void StagingViewport::PublishOutput(UIManager& ui, Rendering::RenderGraph::RGFrame& frame)
{
    const Rendering::TextureHandle output = m_View.OutputTexture();
    if (!output.IsValid())
        return;
    // The output is display-linear: the editor's terminal pass owns the only OETF.
    ui.SetExternalTextureRG(m_TextureName, m_View.OutputWidth(), m_View.OutputHeight(),
                            UI::UITextureSpace::DisplayLinearSdr(), Rendering::TextureFormat::R16G16B16A16_FLOAT);
    const Rendering::RenderGraph::RGTexture texture =
        frame.ImportExternalTexture(m_TextureName.c_str(), output, Rendering::ResourceState::ShaderResource,
                                    Rendering::TextureFormat::R16G16B16A16_FLOAT);
    if (texture.IsValid())
        ui.PublishExternalTextureRG(m_TextureName, frame, texture);
}

void StagingViewport::ReleaseView()
{
    if (m_Tick != RenderGraphTickRegistry::kInvalidHandle)
        m_Ticks.Unregister(m_Tick);
    m_Tick = RenderGraphTickRegistry::kInvalidHandle;
    m_View = {};
    if (UIManager* ui = GetOwnerManager(); ui && !m_TextureName.empty())
        ui->RemoveExternalTexture(m_TextureName);
}

void StagingViewport::ForgetPool()
{
    ReleaseView();
    m_Pool = nullptr;
}
} // namespace GameEngine::Editor
