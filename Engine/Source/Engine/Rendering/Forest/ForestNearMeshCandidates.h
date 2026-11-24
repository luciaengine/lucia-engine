#pragma once

#include "ForestPlacementRuntime.h"

#include <cstddef>
#include <optional>

namespace GameEngine::Rendering { struct GPUMesh; }

namespace GameEngine::Engine::Renderer
{

struct ForestMeshOwnedPlacement;

struct ForestNearMeshCandidate
{
    ForestPageTicket Ticket;
    uint32 LocalPlacement;
    Mathematics::WorldPosition Root;
    float Yaw;
    float Scale;
    double DistanceSquared;
    float Coverage = 0.0f;
};

// Matches ForestMeshTransform in instance_io.glsl.
struct alignas(16) ForestMeshTransform
{
    Mathematics::Vector4 LocalAndScale;
    float YawSin;
    float YawCos;
    uint32 SectorPacked[2];
};
static_assert(sizeof(ForestMeshTransform) == 32);
static_assert(offsetof(ForestMeshTransform, YawSin) == 16);
static_assert(offsetof(ForestMeshTransform, SectorPacked) == 24);

std::optional<ForestMeshTransform> PackForestMeshTransform(const ForestNearMeshCandidate& tree);

struct ForestNearMeshView
{
    ForestPlacementView PlacementView;
    float ProjectionScaleY;
    float GlobalLodBias;
    float ImpostorCoverage;
    float DeformationMargin = 0.0f; // Source mesh offset in model meters, used only for visibility.
    bool Orthographic = false;
};

// Exclusive card-entry boundary for this part in model-sphere coverage space.
float ForestPartCardEntryCoverage(const Rendering::GPUMesh& mesh, float defaultSse, float tightSse);
uint32 SelectForestMeshLod(const Rendering::GPUMesh& mesh, float modelCoverage,
                           float defaultSse, float tightSse, uint32 forceLod);

/// Collects at most maxCandidates nearest trees inside the card threshold of view, nearest first.
/// previousOwners, sorted by ticket and local placement, keep their mesh inside the switch band.
/// A candidate replaces its card only after its complete mesh submission is ready.
void CollectForestNearMeshCandidates(const ForestPlacementRuntime& runtime,
    std::span<const ForestDetailDraw> draws, const ForestNearMeshView& view, const Mathematics::Vector4& modelSphere,
    size_t maxCandidates, std::vector<ForestNearMeshCandidate>& output,
    std::span<const ForestMeshOwnedPlacement> previousOwners = {});

} // namespace GameEngine::Engine::Renderer
