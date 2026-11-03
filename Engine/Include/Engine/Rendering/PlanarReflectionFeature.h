#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/PlanarReflectionMath.h"
#include "Engine/Rendering/MirrorCaptureView.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include <map>
#include <mutex>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace Pipeline { struct ViewDeclare; }

// Independent scene captures, keyed by world, main view and mirror entity.
// No cubemaps, probe selection or environment-source ownership lives here.
class PlanarReflectionFeature final : public IRenderFeature
{
public:
    ~PlanarReflectionFeature() override;
    void SyncWorld(RenderServices& services, uint64_t worldId,
                   std::vector<PlanarReflectionPlane> planes);
    bool HasPlanes(const Rendering::ViewDesc& view) const;
    std::vector<PlanarReflectionPlane> GetPlanes(const Rendering::ViewDesc& view) const;
    struct Capture
    {
        Rendering::RenderGraph::RGTexture Color{};
        Rendering::CameraData Camera{};
    };
    Capture DeclareCapture(Pipeline::ViewDeclare& d, const PlanarReflectionPlane& plane);

private:
    struct CaptureKey
    {
        uint64_t World = 0;
        uint32_t MainView = 0;
        uint32_t Entity = 0;
        auto operator<=>(const CaptureKey&) const = default;
    };
    using CaptureView = MirrorCaptureView;
    void Release(CaptureView& capture);
    RenderServices* m_Services = nullptr;
    mutable std::mutex m_Mutex;
    std::map<uint64_t, std::vector<PlanarReflectionPlane>> m_Planes;
    std::map<CaptureKey, CaptureView> m_Captures;
};
} // namespace GameEngine::Engine::Renderer
