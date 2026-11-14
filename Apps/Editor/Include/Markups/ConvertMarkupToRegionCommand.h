#pragma once

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector2.h"
#include "Spline/SplineTypes.h"
#include "UndoRedo/IEditorCommand.h"

#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

// One undo step that turns a volume mark-up into a region, one way: the entity keeps its
// Markup, its title, its notes, its status and its thread, loses its MarkupVolume and gains a
// closed spline over `knots` (world x, z) with a MarkupRegion `extrudeHeight` tall; it moves to
// the knots' label point, unrotated and unscaled (the knots carry the shape). Undo gives the
// volume and its transform back; redo puts the same spline back. The spline the command
// creates lives as long as the command holds it applied, and is destroyed with a command that
// was undone.
class ConvertMarkupToRegionCommand final : public IEditorCommand
{
  public:
    ConvertMarkupToRegionCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                                 EditorChangeNotifications* notifications, std::vector<Mathematics::Vector2> knots,
                                 Spline::SplineType type, float32 extrudeHeight);
    ~ConvertMarkupToRegionCommand() override;

    ConvertMarkupToRegionCommand(const ConvertMarkupToRegionCommand&) = delete;
    ConvertMarkupToRegionCommand& operator=(const ConvertMarkupToRegionCommand&) = delete;

    const char* GetName() const override { return m_Label.c_str(); }
    const char* GetTypeName() const override { return "ConvertMarkupToRegionCommand"; }
    void Do() override;
    void Undo() override;

  private:
    void Announce();

    std::string m_Label;
    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    EditorChangeNotifications* m_Notifications;
    std::vector<Mathematics::Vector2> m_Knots;
    Spline::SplineType m_Type;
    float32 m_ExtrudeHeight;

    // The volume as it was.
    Components::Transform m_TransformBefore{};
    Components::WorldTransform m_WorldBefore{};
    Components::MarkupVolume m_VolumeBefore{};
    // The region as the first Do made it.
    bool m_Made = false;
    bool m_Applied = false;
    Components::Transform m_TransformAfter{};
    Components::WorldTransform m_WorldAfter{};
    Components::SplineComponent m_Spline{};
    Components::MarkupRegion m_Region{};
};

} // namespace GameEngine::Editor
