#include "TerrainECS/TerrainUploadNode.h"

#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::TerrainECS
{

bool TerrainUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    return true;
}

void TerrainUploadNode::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& declaration)
{
    namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
    auto& services = declaration.Services;
    auto* device = services.GetDevice();
    if (!device)
        return;

    auto& feature = services.EnsureFeature<TerrainRenderFeature>();
    if (!feature.IsInitialized() && !feature.Initialize(device))
        return;

    // Retires deferred texture/buffer destroys (ReleaseTerrainResources) once per frame.
    // services frees each retired terrain texture's bindless slot before its image is destroyed.
    feature.TryCleanupStaleViews(device->GetFrameIndex(), services);

    // This upload stream uses its monotonic graph-frame key. Consumers add dependencies within
    // that graph; independently clocked streams require explicit producer ownership.
    const uint64 rgFrame = declaration.Frame.FrameIndex();

    // Page payloads stay pending until their graph copy records and submits.
    auto& pages = services.EnsureFeature<TerrainHeightPageFeature>();
    pages.Latch(*device, rgFrame);

    // One graphics upload per frame. Legacy height/splat/normal textures keep explicit consumer
    // edges; the paged cache declares its own read/write hazards in the graph.
    if ((feature.HasPendingUploads() || feature.HasPendingBakes() || pages.HasStagedUploads()) &&
        feature.TryClaimHeightmapUploadFrameRG(rgFrame, &declaration.Frame))
    {
        const RenderGraph::RGPass uploadPass = declaration.Frame.AddPass(
            "TerrainHeightmapUpload", Rendering::PassPhase::kEarlySetup,
            [&pages, frame = &declaration.Frame](RenderGraph::RGPassBuilder& pass)
            {
                pass.PreventCulling();
                pages.DeclareUploads(*frame, pass);
            },
            [featurePointer = &feature, pagesPointer = &pages, frame = &declaration.Frame](RenderGraph::RGContext& context)
            {
                if (!context.Cmd)
                    return;
                // Legacy slot copies precede height bakes in this command list.
                // Their bake barriers expose the resulting height to compute consumers.
                featurePointer->FlushPendingUploads(context.Cmd);
                featurePointer->FlushPendingBakes(context.Cmd);
                pagesPointer->FlushUploads(*context.Cmd, *frame);
            });

        // Consumers pull this and order themselves after it. This node never learns who they
        // are — the dependency belongs to whoever samples the textures.
        feature.RecordHeightmapUploadPassRG(rgFrame, &declaration.Frame, uploadPass.Id);
    }
}

} // namespace GameEngine::TerrainECS
