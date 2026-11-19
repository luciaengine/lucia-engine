#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Rendering
{
struct GPUInstance;
}

namespace GameEngine::Engine::Renderer
{

/// The largest screen coverage of each material's instances. A sweep reads the scene's instance
/// rows a bounded number per call, so the cost per frame does not grow with the scene.
///
/// Coverage is radius * projScaleY / distance, the ge_SelectLOD metric, with the distance taken to
/// the sphere's surface: a camera inside a large mesh gives that mesh the highest demand. There is
/// no frustum test, so a surface behind the camera keeps its demand through a turn.
class MaterialScreenDemand
{
public:
    struct View
    {
        Mathematics::Vector3Int OriginSector{0, 0, 0}; ///< The view's render origin sector.
        Mathematics::Vector3 Eye{};                    ///< The eye, relative to OriginSector.
        float32 ProjScaleY = 0.0f;                     ///< |proj[1][1]| of a perspective view.
    };

    /// Reads up to `rowBudget` rows. Returns true when the call completes a sweep: Coverage()
    /// then holds the table of that sweep.
    bool Advance(std::span<const Rendering::GPUInstance> rows, const View& view, uint32 rowBudget);

    /// Coverage by GPUScene material index, from the last completed sweep. A material with no live
    /// row has 0, or lies past the end.
    std::span<const float32> Coverage() const { return m_Published; }

    /// Drops the table and the sweep in progress.
    void Reset();

private:
    std::vector<float32> m_Building;
    std::vector<float32> m_Published;
    size_t m_Cursor = 0;
};

} // namespace GameEngine::Engine::Renderer
