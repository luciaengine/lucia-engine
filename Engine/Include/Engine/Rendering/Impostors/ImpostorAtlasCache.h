#pragma once

#include "Mathematics/Octahedral.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <compare>
#include <vector>

namespace GameEngine::Engine::Renderer
{

/// What an impostor atlas is baked from and how: equal keys share one atlas.
struct ImpostorAtlasKey
{
    uint64 MeshHash = 0;      // Content hash of the meshes the atlas shows.
    uint64 MaterialHash = 0;  // Hash of the materials they draw with.
    Mathematics::OctahedralLayout Layout = Mathematics::OctahedralLayout::Hemisphere;
    uint32 FramesPerSide = 8; // The atlas holds FramesPerSide^2 frames.
    uint32 FrameSize = 128;   // Pixels per frame side.
    bool BakeDiffuseTransmission = false;
    bool FilterMips = false;

    auto operator<=>(const ImpostorAtlasKey&) const = default;
};

/// Allocated mip levels through a four-texel view tile when filtering is enabled.
uint32 ImpostorAtlasMipCount(const ImpostorAtlasKey& key);

/// GPU bytes of all allocated atlas levels, including optional diffuse transmission.
uint64 ImpostorAtlasBytes(const ImpostorAtlasKey& key);

/// Which atlases live inside a byte budget. An atlas is requested when an instance first needs
/// it; past the budget the least recently used atlases go, and their instances draw the mesh.
/// Bookkeeping only: the owner creates, bakes and frees the GPU textures it names.
class ImpostorAtlasCache
{
public:
    enum class State : uint8
    {
        Absent,  // Not held: Request it.
        Pending, // Held, waiting for its bake.
        Ready,   // Baked: instances may draw the impostor.
    };

    explicit ImpostorAtlasCache(uint64 budgetBytes);

    State GetState(const ImpostorAtlasKey& key) const;
    bool HasRecentPendingRequest(const ImpostorAtlasKey& key, uint64 frame) const;

    /// Holds `key` and marks it used in `frame`. An absent atlas becomes Pending when it fits the
    /// budget after evicting atlases unused in both the current and previous frame.
    /// Failed admissions retain the existing atlases. Evicted keys go to `outEvicted`.
    /// All callers share one frame epoch.
    /// Returns the state after the request: Absent when the atlas does not fit.
    State Request(const ImpostorAtlasKey& key, uint64 frame, std::vector<ImpostorAtlasKey>& outEvicted);

    /// The bake of a Pending atlas finished.
    void MarkReady(const ImpostorAtlasKey& key);

    /// Return a failed bake's reservation to the budget.
    void ReleasePending(const ImpostorAtlasKey& key);

    /// Drops every atlas, for a device rebuild that freed their textures.
    void Clear();

    uint64 GetUsedBytes() const { return m_UsedBytes; }
    uint64 GetBudgetBytes() const { return m_BudgetBytes; }

private:
    struct Entry
    {
        State Status = State::Pending;
        uint64 LastUsedFrame = 0;
    };

    bool TryEvictLeastRecentlyUsed(uint64 neededBytes, uint64 frame, std::vector<ImpostorAtlasKey>& outEvicted);

    uint64 m_BudgetBytes = 0;
    uint64 m_UsedBytes = 0;
    FlatMap<ImpostorAtlasKey, Entry> m_Entries;
};

} // namespace GameEngine::Engine::Renderer
