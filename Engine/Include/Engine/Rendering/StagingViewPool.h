#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/StagingView.h"
#include "Types/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class RenderServices;

/// The first size, in device pixels, and the debug name of a staging view. A StagingViewport control then sets
/// the size from its layout; a view without a control keeps this size.
struct StagingViewDesc
{
    uint32 Width = 256;
    uint32 Height = 256;
    const char* DebugName = "Staging View";
};

/**
 * @brief Pooled ECS worlds, each with an OnDemand view that renders into a display-linear RGBA16F output.
 * One pool per RenderServices (`rs.EnsureFeature<StagingViewPool>(rs)`); RenderServices destroys it at Shutdown.
 * A host acquires a StagingView and works through it. Main thread only, outside a system or a job.
 */
class StagingViewPool final : public IRenderFeature
{
  public:
    explicit StagingViewPool(RenderServices& renderServices);
    /// Frees every entry's world, view and output. A StagingView that is still alive becomes empty.
    ~StagingViewPool() override;
    StagingViewPool(const StagingViewPool&) = delete;
    StagingViewPool& operator=(const StagingViewPool&) = delete;

    /// Takes a free entry or creates one; its world is empty and its camera has the default pose.
    /// The returned view owns the entry until it is destroyed or assigned.
    StagingView Acquire(const StagingViewDesc& desc);
    /// Sets the render size of `view` in device pixels; the output takes it on the next declared frame.
    /// The StagingViewport control calls it from its layout, so a host does not.
    void Resize(StagingView& view, uint32 width, uint32 height);

  private:
    friend class StagingView;
    struct Entry;

    // StagingView forwards to these; each requires a live handle.
    // Release destroys every entity in the entry's world, frees its output and returns the entry to the pool.
    // At its 255th release the entry retires and frees its world, view and camera.
    void Release(StagingViewHandle handle);
    bool IsAlive(StagingViewHandle handle) const;
    // The view that holds the entry: the destructor empties it.
    void SetOwner(StagingViewHandle handle, StagingView* owner);
    ECS::World& World(StagingViewHandle handle);
    Rendering::ViewId View(StagingViewHandle handle) const;
    void SetCamera(StagingViewHandle handle, const Camera& camera);
    uint32 RenderWidth(StagingViewHandle handle) const;
    uint32 RenderHeight(StagingViewHandle handle) const;
    StagingWorldPassRG DeclareWorldPass(StagingViewHandle handle, Rendering::RenderGraph::RGFrame& frame,
                                        Rendering::MaterialKeyword keywords);
    bool DeclareOutput(StagingViewHandle handle, Rendering::RenderGraph::RGFrame& frame,
                       Rendering::RenderGraph::RGTexture source, const Rendering::Passes::TonemapParams& params);
    const std::string& UiTextureName(StagingViewHandle handle) const;
    Rendering::TextureHandle OutputTexture(StagingViewHandle handle) const;
    uint32 OutputWidth(StagingViewHandle handle) const;
    uint32 OutputHeight(StagingViewHandle handle) const;
    uint64 OutputRevision(StagingViewHandle handle) const;

    Entry& Resolve(StagingViewHandle handle);
    const Entry& Resolve(StagingViewHandle handle) const;
    void WriteCameraData(Entry& entry);
    void UpdateTargetNames(Entry& entry);
    bool IsOutputCurrent(const Entry& entry) const;
    void ForgetOutputIfDeviceRebuilt(Entry& entry);
    bool EnsureOutputTexture(Entry& entry);
    void DestroyOutputTexture(Entry& entry);
    void RetireEntry(Entry& entry);

    RenderServices& m_RenderServices;
    // Owns the index and the generation; an entry sits at its handle's index.
    Rendering::HandleManager m_Handles;
    std::vector<std::unique_ptr<Entry>> m_Entries;
};
} // namespace GameEngine::Engine::Renderer
