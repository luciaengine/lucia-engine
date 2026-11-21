#include "Engine/Rendering/Forest/ForestNearMeshCandidates.h"

#include "Assets/HlodSelect.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Common/Frustum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace GameEngine::Engine::Renderer
{
namespace
{

bool Nearer(const ForestNearMeshCandidate& a, const ForestNearMeshCandidate& b)
{
    return std::tie(a.DistanceSquared, a.Ticket.Page, a.LocalPlacement) <
           std::tie(b.DistanceSquared, b.Ticket.Page, b.LocalPlacement);
}

double PageDistanceSquared(const PageStreaming::PlacementPage& page, const Mathematics::WorldPosition& eye)
{
    const std::array<double, 3> eyeLocal{eye.X - page.Origin[0], eye.Y - page.Origin[1], eye.Z - page.Origin[2]};
    double distance = 0;
    for (size_t axis = 0; axis < eyeLocal.size(); ++axis)
    {
        const double edge = std::clamp(eyeLocal[axis], static_cast<double>(page.LocalMin[axis]),
                                      static_cast<double>(page.LocalMax[axis]));
        const double delta = edge - eyeLocal[axis];
        distance += delta * delta;
    }
    return distance;
}

std::array<float, Rendering::kMaxMeshLODs - 1u> PartThresholds(
    const Rendering::GPUMesh& mesh, uint32 count, float defaultSse, float tightSse)
{
    std::array<float, Rendering::kMaxMeshLODs - 1u> thresholds{};
    const float sseScale = (mesh.lodFlags & Rendering::kGPUMeshLodTightClassBit) != 0 ? tightSse : defaultSse;
    float previous = Rendering::kLodEffNone;
    for (uint32 level = 0; level + 1u < count; ++level)
    {
        previous = Rendering::LodEffectiveThreshold(mesh.lodThreshold[level], (mesh.lodFlags & (1u << level)) != 0,
                                                     sseScale, mesh.lodSseScaleCeil, previous);
        thresholds[level] = previous;
    }
    return thresholds;
}

} // namespace

std::optional<ForestMeshTransform> PackForestMeshTransform(const ForestNearMeshCandidate& tree)
{
    constexpr double limit = (static_cast<double>(Rendering::kSectorAxisMax) + 1.5) * Mathematics::kWorldSectorSize;
    if (!(std::abs(tree.Root.X) < limit && std::abs(tree.Root.Y) < limit && std::abs(tree.Root.Z) < limit) ||
        !std::isfinite(tree.Yaw) || !std::isfinite(tree.Scale) || tree.Scale <= 0.0f)
        return std::nullopt;

    Mathematics::Vector3Int sector;
    Mathematics::Vector3 local;
    tree.Root.ToSector(sector, local);
    for (const int32 axis : {sector.X, sector.Y, sector.Z})
        if (axis < Rendering::kSectorAxisMin || axis > Rendering::kSectorAxisMax)
            return std::nullopt;

    ForestMeshTransform row{
        {local.x, local.y, local.z, tree.Scale}, std::sin(tree.Yaw), std::cos(tree.Yaw), {}};
    Rendering::PackSector(sector.X, sector.Y, sector.Z, row.SectorPacked[0], row.SectorPacked[1]);
    return row;
}

float ForestPartCardEntryCoverage(const Rendering::GPUMesh& mesh, float defaultSse, float tightSse)
{
    const uint32 count = std::clamp(mesh.lodCount, 1u, Rendering::kMaxMeshLODs);
    if (count == 1u)
        return Rendering::kLodEffNone;
    if (!(mesh.lodCoverageScale > 0.0f) || !std::isfinite(mesh.lodCoverageScale))
        return 0.0f;

    const auto thresholds = PartThresholds(mesh, count, defaultSse, tightSse);
    const float threshold = thresholds[count - 2u];
    const float margin = 1.0f + ImpostorStage::kSwitchHysteresis;
    return std::max(0.0f, threshold / (mesh.lodCoverageScale * margin));
}

uint32 SelectForestMeshLod(const Rendering::GPUMesh& mesh, float modelCoverage,
                           float defaultSse, float tightSse, uint32 forceLod)
{
    const uint32 count = std::clamp(mesh.lodCount, 1u, Rendering::kMaxMeshLODs);
    if (forceLod != 0xFFFFFFFFu)
        return std::min(forceLod, count - 1u);
    const float coverage = modelCoverage * mesh.lodCoverageScale;
    const auto thresholds = PartThresholds(mesh, count, defaultSse, tightSse);
    for (uint32 level = 0; level + 1u < count; ++level)
        if (coverage >= thresholds[level])
            return level;
    return count - 1u;
}

void CollectForestNearMeshCandidates(const ForestPlacementRuntime& runtime,
    const ForestPlacementFrame& frame, const ForestNearMeshView& view, const Mathematics::Vector4& modelSphere,
    size_t maxCandidates, std::vector<ForestNearMeshCandidate>& output,
    std::span<const ForestMeshOwnedPlacement> previousOwners)
{
    output.clear();
    if (maxCandidates == 0 || !std::isfinite(modelSphere.w) || modelSphere.w <= 0 ||
        !std::isfinite(view.ImpostorCoverage) || view.ImpostorCoverage < 0 ||
        !std::isfinite(view.DeformationMargin) || view.DeformationMargin < 0 ||
        !std::isfinite(view.ProjectionScaleY) || view.ProjectionScaleY <= 0 || !std::isfinite(view.GlobalLodBias))
        return;

    const auto& eye = view.PlacementView.Eye;
    const auto archive = runtime.Store().Pages();
    // Authored page bounds contain the model sphere center at every scale and yaw.
    // Round the broadphase outward so a page at the threshold takes the exact test.
    constexpr double kCoverageMargin = 1.00001;
    const double maxDistance = view.ImpostorCoverage == 0.0f ? std::numeric_limits<double>::infinity() :
        std::max(1.0e-4, modelSphere.w * PageStreaming::PackedPlacement::kMaxScale *
            view.ProjectionScaleY * std::exp2(static_cast<double>(view.GlobalLodBias)) /
            view.ImpostorCoverage) * kCoverageMargin;
    const float coverageBias = std::exp2(view.GlobalLodBias);
    const float meshEnterCoverage = view.ImpostorCoverage * (1.0f + ImpostorStage::kSwitchHysteresis);
    for (const auto& detail : frame.DetailedDraws)
    {
        if (detail.FadeCode != 0)
            continue;
        const auto records = runtime.ReadyPlacements(detail.Ticket);
        if (records.empty() || PageDistanceSquared(archive[detail.Ticket.Page], eye) > maxDistance * maxDistance)
            continue;

        const auto& origin = archive[detail.Ticket.Page].Origin;
        for (uint32 row = 0; row < records.size(); ++row)
        {
            const auto& packed = records[row];
            const float scale = static_cast<float>(packed.UniformScale());
            const float yaw = static_cast<float>(packed.YawRadians());
            const float sinYaw = std::sin(yaw);
            const float cosYaw = std::cos(yaw);
            const Mathematics::WorldPosition root{origin[0] + packed.LocalX(), origin[1], origin[2] + packed.LocalZ()};
            const Mathematics::Vector3 centerOffset{
                modelSphere.x * cosYaw + modelSphere.z * sinYaw, modelSphere.y,
                -modelSphere.x * sinYaw + modelSphere.z * cosYaw};
            const Mathematics::Vector3 center = root - eye + centerOffset * scale;
            const double distanceSquared = static_cast<double>(center.x) * center.x +
                static_cast<double>(center.y) * center.y + static_cast<double>(center.z) * center.z;
            const float radius = scale * modelSphere.w;
            const float coverage = Hlod::SphereCoverageAtDistance(
                std::sqrt(distanceSquared), radius, view.ProjectionScaleY, coverageBias);
            const float frustumRadius = scale * (modelSphere.w + view.DeformationMargin);
            if (coverage < view.ImpostorCoverage ||
                (coverage < meshEnterCoverage &&
                 !std::ranges::binary_search(previousOwners, ForestMeshOwnedPlacement{detail.Ticket, row})) ||
                !Rendering::TestSphereFrustum(center, frustumRadius, view.PlacementView.EyeRelativePlanes.data()))
                continue;
            const ForestNearMeshCandidate candidate{detail.Ticket, row, root, yaw, scale,
                                                     distanceSquared, coverage};
            if (output.size() < maxCandidates)
            {
                output.push_back(candidate);
                std::push_heap(output.begin(), output.end(), Nearer);
            }
            else if (Nearer(candidate, output.front()))
            {
                std::pop_heap(output.begin(), output.end(), Nearer);
                output.back() = candidate;
                std::push_heap(output.begin(), output.end(), Nearer);
            }
        }
    }
    std::sort_heap(output.begin(), output.end(), Nearer);
}

} // namespace GameEngine::Engine::Renderer
