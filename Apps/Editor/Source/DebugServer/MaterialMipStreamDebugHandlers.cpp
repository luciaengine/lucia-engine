#include "DebugServer/MaterialMipStreamDebugHandlers.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace GameEngine
{

using json = nlohmann::json;
using StreamState = Engine::Renderer::TextureService::MaterialMipStreamState;

namespace
{

// The longest edge of the first resident level, in texels.
uint32_t ResidentExtent(const StreamState& stream)
{
    return std::max(stream.TopExtent >> stream.FirstMip, 1u);
}

// The value that MaterialMipStepPlanner orders by: screen coverage per resident texel.
double Need(const StreamState& stream)
{
    return static_cast<double>(stream.ScreenCoverage) / static_cast<double>(ResidentExtent(stream));
}

bool NeedsMore(const StreamState& left, const StreamState& right)
{
    return Need(left) > Need(right);
}

json DescribeStream(const StreamState& stream, AssetManager* assets)
{
    json item = {
        {"texture", stream.Texture.ToString()},
        {"firstMip", stream.FirstMip},
        {"tail", stream.Tail},
        {"topExtent", stream.TopExtent},
        {"residentExtent", ResidentExtent(stream)},
        {"residentBytes", stream.ResidentBytes},
        {"screenCoverage", stream.ScreenCoverage},
        {"need", Need(stream)},
        {"keepsLevel", stream.KeepsLevel},
    };
    if (stream.StepTargetMip)
        item["stepTargetMip"] = *stream.StepTargetMip;
    if (AssetMetadata meta; assets && assets->GetRegistry().TryGetAssetMetadata(stream.Texture, meta))
        item["path"] = meta.Path.generic_string();
    return item;
}

json HandleMaterialMipStreams()
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return Editor::RefuseRequest("No render services: the renderer has not started");
    Engine::Renderer::TextureService::MaterialMipStreamReport report =
        renderServices->Textures().ReportMaterialMipStreams();
    std::ranges::stable_sort(report.Streams, NeedsMore);

    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    uint64_t residentBytes = 0;
    json streams = json::array();
    for (const StreamState& stream : report.Streams)
    {
        residentBytes += stream.ResidentBytes;
        streams.push_back(DescribeStream(stream, assets));
    }
    return json{{"enabled", report.BudgetBytes != 0},
                {"budgetBytes", report.BudgetBytes},
                {"residentBytes", residentBytes},
                {"streams", std::move(streams)}};
}

} // namespace

void RegisterMaterialMipStreamDebugHandlers(EditorDebugServer& server)
{
    // get_material_mip_streams — every material mip stream now, most need first. No parameters.
    server.RegisterHandler("get_material_mip_streams",
                           [](const EditorDebugServer::RequestContext&) -> json { return HandleMaterialMipStreams(); });
}

} // namespace GameEngine
