#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Rendering
{
struct CameraData;
struct GPUInstance;
}

namespace GameEngine::Engine::Renderer
{

/// The largest screen coverage of each material's instances. A sweep reads the scene's instance
/// rows a bounded number per update, so the cost per frame does not grow with the scene.
///
/// Perspective coverage is radius * projScaleY / distance, the ge_SelectLOD metric, with the
/// distance taken to the sphere's surface: a camera inside a large mesh gives that mesh the highest
/// demand. Orthographic coverage is radius * projScaleY. There is no frustum test, so a surface
/// behind the camera keeps its demand through a turn.
class MaterialScreenDemand
{
public:
    struct View
    {
        Mathematics::Vector3Int OriginSector{0, 0, 0}; ///< The view's render origin sector.
        Mathematics::Vector3 Eye{};                    ///< The eye, relative to OriginSector.
        float32 ProjScaleY = 0.0f;                     ///< |proj[1][1]|.
        bool Orthographic = false;
    };

    struct Pace
    {
        uint32 RowsPerUpdate = 0;      ///< The most rows that one update reads.
        uint32 MinUpdatesPerSweep = 1; ///< A sweep of a small scene still takes this many updates.
    };

    /// The view of `camera`. Nullopt for a camera with no eye (RenderOrigin.h SetCameraEye).
    static std::optional<View> ViewOf(const Rendering::CameraData& camera);

    /// One update of the sweep. Returns true when it completes a sweep whose table differs from
    /// the last one: Coverage() then holds the new table. A still view of a still scene returns
    /// true one time.
    bool Advance(std::span<const Rendering::GPUInstance> rows, const View& view, const Pace& pace);

    /// Coverage by GPUScene material index, from the last completed sweep. A material with no live
    /// row has 0, or lies past the end.
    std::span<const float32> Coverage() const { return m_Published; }

    /// Drops the table and the sweep in progress.
    void Reset();

private:
    std::vector<float32> m_Building;
    std::vector<float32> m_Published;
    size_t m_Cursor = 0;
    uint32 m_UpdatesInSweep = 0;
};

} // namespace GameEngine::Engine::Renderer
