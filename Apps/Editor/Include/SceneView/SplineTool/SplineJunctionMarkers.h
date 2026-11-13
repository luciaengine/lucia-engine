#pragma once

#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewGizmos.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <chrono>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SplineECS
{
struct SplineNode;
}

namespace GameEngine::Editor::SceneTools
{

class SplineInteractionState;

/// The Scene View's junction markers: a ring on a dark plate with the kind's glyph at every node of
/// the spline network, white when joined, amber when the heights differ, teal for a short while
/// after a junction forms; and the teal ring where a dragged or placed end snaps. The ring is
/// 1.25 times a knot's pick radius. They draw, and answer the pointer, with every tool.
class SplineJunctionMarkers
{
public:
    void Render(GizmoRenderContext& context, const ECS::World& world, SplineInteractionState& state);

    /// Picks the marker under `ray` and keeps its hover text. Render picks again with this ray
    /// every frame, so the text follows a junction that forms, changes or goes under a still pointer;
    /// the text is built again only when the marker, its state or a name in it changes.
    void UpdateHover(const ECS::World& world, const GizmoRay& ray);

    /// The hover text of the marker under the pointer; empty for none.
    const std::string& GetHoverText() const { return m_HoverText; }

    /// The marker's hover text: the kind, the pair and the state, in the inspector's words.
    static std::string Tooltip(const ECS::World& world, const SplineECS::SplineNode& node);

private:
    struct Seen
    {
        std::chrono::steady_clock::time_point First{};
        uint64 Frame = 0;
    };

    // Picks with the kept ray, writes the hovered node, and builds its text when its key changed.
    void RefreshHover(const ECS::World& world);
    // The id of the node whose marker the ray passes through, the nearest to the eye; 0 for none.
    uint32 Pick(const ECS::World& world, const GizmoRay& ray);

    // When each node id was first drawn; a node first drawn in the world's first frame is old.
    FlatMap<uint32, Seen> m_Seen;
    uint64 m_Frame = 0;
    uint64 m_WorldId = 0;
    uint64 m_WorldGeneration = 0;
    bool m_Primed = false;

    // Reused by every pick, so a pointer move allocates nothing once it has grown.
    std::vector<GizmoIconPickSphere> m_PickSpheres;
    GizmoRay m_HoverRay{};
    bool m_HasHoverRay = false;
    uint32 m_HoveredNodeId = 0;
    std::string m_HoverText;
    // The key of the values m_HoverText was built from; valid while m_HasHoverText.
    uint64 m_HoverTextKey = 0;
    bool m_HasHoverText = false;
};

} // namespace GameEngine::Editor::SceneTools
