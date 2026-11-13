#include "Engine/Rendering/StagingView.h"

#include "Engine/Rendering/StagingViewPool.h"

#include <utility>

namespace GameEngine::Engine::Renderer
{
StagingView::StagingView(StagingViewPool& pool, StagingViewHandle handle)
    : m_Pool(&pool)
    , m_Handle(handle)
{
    m_Pool->SetOwner(m_Handle, this);
}

StagingView::~StagingView()
{
    Release();
}

StagingView::StagingView(StagingView&& other) noexcept
    : m_Pool(std::exchange(other.m_Pool, nullptr))
    , m_Handle(std::exchange(other.m_Handle, StagingViewHandle{}))
{
    if (m_Pool)
        m_Pool->SetOwner(m_Handle, this);
}

StagingView& StagingView::operator=(StagingView&& other) noexcept
{
    if (this == &other)
        return *this;
    Release();
    m_Pool = std::exchange(other.m_Pool, nullptr);
    m_Handle = std::exchange(other.m_Handle, StagingViewHandle{});
    if (m_Pool)
        m_Pool->SetOwner(m_Handle, this);
    return *this;
}

bool StagingView::IsAlive() const
{
    return m_Pool && m_Pool->IsAlive(m_Handle);
}

ECS::World& StagingView::World()
{
    return m_Pool->World(m_Handle);
}

Rendering::ViewId StagingView::ViewId() const
{
    return m_Pool->View(m_Handle);
}

void StagingView::SetCamera(const Camera& camera)
{
    m_Pool->SetCamera(m_Handle, camera);
}

uint32 StagingView::RenderWidth() const
{
    return m_Pool->RenderWidth(m_Handle);
}

uint32 StagingView::RenderHeight() const
{
    return m_Pool->RenderHeight(m_Handle);
}

StagingWorldPassRG StagingView::DeclareWorldPass(Rendering::RenderGraph::RGFrame& frame,
                                                 Rendering::MaterialKeyword keywords)
{
    return m_Pool->DeclareWorldPass(m_Handle, frame, keywords);
}

bool StagingView::DeclareOutput(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture source,
                                const Rendering::Passes::TonemapParams& params)
{
    return m_Pool->DeclareOutput(m_Handle, frame, source, params);
}

const std::string& StagingView::UiTextureName() const
{
    return m_Pool->UiTextureName(m_Handle);
}

Rendering::TextureHandle StagingView::OutputTexture() const
{
    return m_Pool->OutputTexture(m_Handle);
}

uint32 StagingView::OutputWidth() const
{
    return m_Pool->OutputWidth(m_Handle);
}

uint32 StagingView::OutputHeight() const
{
    return m_Pool->OutputHeight(m_Handle);
}

uint64 StagingView::OutputRevision() const
{
    return m_Pool->OutputRevision(m_Handle);
}

void StagingView::Release()
{
    if (m_Pool && m_Pool->IsAlive(m_Handle))
        m_Pool->Release(m_Handle);
    Detach();
}

void StagingView::Detach()
{
    m_Pool = nullptr;
    m_Handle = {};
}
} // namespace GameEngine::Engine::Renderer
