#include "ForestPlacementSession.h"

#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Rendering/Core/Device.h"

#include <array>

namespace GameEngine::Engine::Renderer
{
using namespace Rendering;

ForestPlacementSession::~ForestPlacementSession()
{
    Reset(false);
}

void ForestPlacementSession::Reset(bool deviceRebuilt)
{
    Stream.reset();
    CountSample = {};
    DetailPlanEpoch.reset();
    if (Device && !deviceRebuilt)
    {
        if (Indices.IsValid())
            Device->DestroyBuffer(Indices);
        if (Slots.IsValid())
            Device->DestroyBuffer(Slots);
    }
    Indices = {};
    Slots = {};
    Cull = {};
    ShadowCulls = {};
    Graph = nullptr;
    Declared = {};
    Planned = {};
    PlanReady = false;
    ShaderRequested = false;
    ShaderAvailable = false;
    EmittedDetails = false;
    EmittedNearMeshes = false;
    EmittedShadows = false;
    WantsCard = false;
    Card.reset();
    NearDemand.clear();
    NearOwners.clear();
    PendingNearOwners.clear();
    PendingNearFrame = {};
}

bool ForestPlacementSession::EnsureGpu(IDevice& device)
{
    Device = &device;
    if (!ShaderRequested)
    {
        ShaderRequested = true;
        ShaderAvailable = Cull.Initialize(device);
        for (auto& cull : ShadowCulls)
            ShaderAvailable = ShaderAvailable && cull.Initialize(device);
    }
    if (!ShaderAvailable)
        return false;
    if (!Slots.IsValid())
    {
        constexpr size_t kSlotBytes = PageStreaming::PackedPlacementStore::kPlacementsPerPage *
                                      sizeof(PageStreaming::PackedPlacement);
        const BufferDesc desc{.size = Placements->SlotCount() * kSlotBytes,
            .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.PlacementSlots"};
        Slots = device.CreateBuffer(desc);
    }
    if (!Indices.IsValid())
    {
        constexpr std::array<uint16, 6> kIndices{0, 1, 2, 3, 4, 5};
        const BufferDesc desc{.size = sizeof(kIndices),
            .usage = static_cast<uint32>(BufferUsage::Index | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "Forest.CardIndices"};
        Indices = device.CreateBuffer(desc);
        if (Indices.IsValid())
            device.UpdateBuffer(Indices, 0, sizeof(kIndices), kIndices.data());
    }
    if (!Slots.IsValid() || !Indices.IsValid())
        return false;
    if (!Stream)
        Stream.emplace(Placements, Slots, Placements->SlotCount());
    return true;
}

} // namespace GameEngine::Engine::Renderer
