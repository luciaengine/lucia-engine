#include "ForestPlacementSession.h"

#include "Engine/Rendering/Forest/ForestSplatCells.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Rendering/Core/Device.h"

#include <array>
#include <vector>

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
        for (const auto buffer : {Indices, Slots, SplatCells, SplatIndices})
            if (buffer.IsValid())
                Device->DestroyBuffer(buffer);
    }
    Indices = {};
    Slots = {};
    SplatCells = {};
    SplatIndices = {};
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

namespace
{
constexpr std::array<uint16, 6> kCardIndices{0, 1, 2, 3, 4, 5};
// Two triangles over the four corner vertices of one splat disk.
constexpr std::array<uint16, 6> kSplatCorners{0, 1, 2, 0, 2, 3};
constexpr uint32 kSplatVertices = 4;

BufferHandle CreateFilledBuffer(IDevice& device, uint32 usage, std::span<const std::byte> bytes, const char* name)
{
    const BufferDesc desc{.size = bytes.size(), .usage = usage | static_cast<uint32>(BufferUsage::TransferDst),
                          .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = name};
    const BufferHandle buffer = device.CreateBuffer(desc);
    if (buffer.IsValid())
        device.UpdateBuffer(buffer, 0, bytes.size(), bytes.data());
    return buffer;
}

BufferHandle CreateIndexBuffer(IDevice& device, const ForestSplatCells* splats)
{
    constexpr auto kIndexUsage = static_cast<uint32>(BufferUsage::Index);
    if (!splats)
        return CreateFilledBuffer(device, kIndexUsage, std::as_bytes(std::span(kCardIndices)), "Forest.CardIndices");
    std::vector<uint16> indices;
    indices.reserve(splats->Cells.size() * kSplatCorners.size());
    for (uint32 cell = 0; cell < splats->Cells.size(); ++cell)
        for (const uint16 corner : kSplatCorners)
            indices.push_back(static_cast<uint16>(cell * kSplatVertices + corner));
    return CreateFilledBuffer(device, kIndexUsage, std::as_bytes(std::span(indices)), "Forest.SplatIndices");
}
} // namespace

uint32 ForestPlacementSession::TreeIndexCount() const
{
    return Splats ? static_cast<uint32>(Splats->Cells.size() * kSplatCorners.size())
                  : static_cast<uint32>(kCardIndices.size());
}

bool ForestPlacementSession::EnsureGpu(IDevice& device)
{
    Device = &device;
    if (!ShaderRequested)
    {
        ShaderRequested = true;
        ShaderAvailable = Cull.Initialize(device, Splats != nullptr);
        for (auto& cull : ShadowCulls)
            ShaderAvailable = ShaderAvailable && cull.Initialize(device, false);
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
        Indices = CreateIndexBuffer(device, nullptr);
    if (Splats && !SplatIndices.IsValid())
        SplatIndices = CreateIndexBuffer(device, Splats.get());
    if (Splats && !SplatCells.IsValid())
        SplatCells = CreateFilledBuffer(device, static_cast<uint32>(BufferUsage::Storage),
                                        std::as_bytes(std::span(Splats->Cells)), "Forest.SplatCells");
    if (!Slots.IsValid() || !Indices.IsValid() || (Splats && (!SplatCells.IsValid() || !SplatIndices.IsValid())))
        return false;
    if (!Stream)
        Stream.emplace(Placements, Slots, Placements->SlotCount());
    return true;
}

} // namespace GameEngine::Engine::Renderer
