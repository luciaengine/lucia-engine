#pragma once

#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Engine::Renderer
{
class SkyEnvironmentSystem;
}
} // namespace GameEngine

namespace GameEngine::Editor
{
class UndoRedoService;

/// Main-thread sky-hour preview; ordinary scrubs leave the authored component unchanged.
/// Closing, sky edits, scene changes and play start clear the preview.
/// An animated clock pauses while held and commits the release hour as one undoable edit.
class DayCycleScrub
{
  public:
    /// `skyGetter` returns the current sky system, or null when rendering has stopped. The world, undo,
    /// notifications and getter captures must outlive this object. `isPlaying` may be null.
    DayCycleScrub(ECS::World& world, std::function<Engine::Renderer::SkyEnvironmentSystem*()> skyGetter,
                  UndoRedoService* undo, EditorChangeNotifications* notifications, std::function<bool()> isPlaying);
    ~DayCycleScrub();
    DayCycleScrub(const DayCycleScrub&) = delete;
    DayCycleScrub& operator=(const DayCycleScrub&) = delete;

    /// Why the scrub is off, as the notice the window shows; empty while the scrub works. The editor
    /// plays, rendering has stopped, the scene has no sky, the sky follows a linked light, or its
    /// sun is set by hand.
    std::string BlockedNotice() const;

    /// The playhead is pressed (true) or released (false).
    void SetHeld(bool held);
    /// The playhead was dragged to `hours` of local solar time. Ignored while the scrub is blocked.
    void ScrubTo(float hours);
    /// The hour the world previews, if any.
    std::optional<float> PreviewHour() const;
    /// Ends the preview when play has started or the scene it was set for is gone: the world was
    /// reset (a scene opened or closed) or its rendered sky is another entity. The window calls this
    /// on its refresh; a world structure change calls it too.
    void CheckScene();

  private:
    /// The current sky system, or null when rendering has stopped.
    Engine::Renderer::SkyEnvironmentSystem* CurrentSky() const;
    void ClearPreview();
    bool IsPlaying() const;
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event);

    ECS::World& m_World;
    std::function<Engine::Renderer::SkyEnvironmentSystem*()> m_SkyGetter;
    UndoRedoService* m_Undo = nullptr;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::function<bool()> m_IsPlaying;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken{};
    EditorChangeNotifications::SubscriptionToken m_StructureToken{};
    // The scene the preview was set for: the rendered sky and the world's reset generation.
    ECS::EntityHandle m_PreviewSky{};
    std::uint64_t m_PreviewResetGeneration = 0;
    bool m_Held = false;
    // Set while the release's own write to the component goes out, which must not end the preview
    // it is ending itself.
    bool m_Releasing = false;
};

} // namespace GameEngine::Editor
