#pragma once

namespace GameEngine
{
namespace UI
{
/// Held modifier keys for one UIManager, reconciled from key events.
///
/// Physical key state is authoritative for the modifier currently changing, so
/// a release callback that still reports the pre-release mask (some Linux
/// paths) cannot leave a modifier stuck. The event's mods mask is the fallback
/// for modifiers already down before this manager began receiving events - a
/// window focused while Shift is held learns Shift from the next key event.
class ModifierKeyState
{
  public:
    /// Reconcile against one key event. `mods` is the platform mask delivered
    /// with the event; `released` distinguishes key-up from press and repeat.
    void ApplyKeyEvent(int key, bool released, int mods);

    /// Reconcile against the platform's live modifier state: a live query, or the mask
    /// on a mouse event. The live state outranks the tracked key state: a modifier the
    /// mask omits is released (its key-up was lost), and one it reports is held.
    void ApplyLiveMask(int mods);

    /// Drop all held state, for the host window's focus-loss boundary.
    /// The window system synthesizes releases only for keys it saw pressed on
    /// that window, so mask-derived state has no release event of its own and
    /// would otherwise leak into later pointer events as a phantom
    /// modifier-click.
    void Reset() { *this = ModifierKeyState{}; }

    /// Same bitmask as UIEvent::Mods.
    int Mask() const;

    bool Shift() const { return m_Shift; }
    bool Alt() const { return m_Alt; }
    bool Ctrl() const { return m_Ctrl; }
    bool Super() const { return m_Super; }
    /// Ctrl on Windows and Linux, Command on macOS.
    bool PrimaryModifier() const { return m_PrimaryModifier; }

  private:
    /// Recompute the derived flags: a modifier is held when either side's key is
    /// physically down, or the platform mask says so. `ignoreMask` drops the
    /// mask bits whose physical state this event just settled authoritatively.
    void Derive(int mods, int ignoreMask);

    bool m_LeftShift = false;
    bool m_RightShift = false;
    bool m_LeftAlt = false;
    bool m_RightAlt = false;
    bool m_LeftCtrl = false;
    bool m_RightCtrl = false;
    bool m_LeftSuper = false;
    bool m_RightSuper = false;

    bool m_Shift = false;
    bool m_Alt = false;
    bool m_Ctrl = false;
    bool m_Super = false;
    bool m_PrimaryModifier = false;
};
} // namespace UI
} // namespace GameEngine
