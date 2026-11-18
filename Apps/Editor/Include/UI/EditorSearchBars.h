#pragma once

#include <vector>

namespace GameEngine {

class UIElement;

// Central place for editor-wide search bar visibility.
// Panels can register their search bar root element to allow live show/hide updates.
// Visibility setting is persisted in Editor preferences (Preferences.json).
//
// IMPORTANT: Registered elements MUST be unregistered before they are destroyed
// (e.g. in panel destructor or before rebuilding UI), since this stores raw pointers.
// Hint targets (RegisterHintTarget) are the exception: they are held weakly.
class EditorSearchBars {
public:
    static bool GetVisible();
    static void SetVisible(bool visible);
    static bool GetAtTop();
    static void SetAtTop(bool atTop);
    static bool GetAccentFocusOutline();
    static void SetAccentFocusOutline(bool enabled);

    // The look of a registered search bar: the editor's own, or a field fill with a subtle border.
    enum class Look
    {
        Lucia,
        Classic,
    };
    static Look GetLook();
    static void SetLook(Look look);

    // The icon at the left of a search row and the "Search" hint of an empty field. Off by default.
    static bool GetShowIconAndHint();
    static void SetShowIconAndHint(bool show);

    static void Register(UIElement* searchBar);
    // A search bar that sits inside a host row, where the host is what the top/bottom
    // setting moves: visibility and focus styling follow the search bar, the placement
    // classes go on the host (search-bar-at-top on it, search-bars-at-top on its parent).
    static void RegisterHosted(UIElement* searchBar, UIElement* placementHost);
    static void Unregister(UIElement* searchBar);
    static void RegisterFocusTarget(UIElement* searchBar);
    static void UnregisterFocusTarget(UIElement* searchBar);
    // An icon at the left of a search row. It is hidden while ShowIconAndHint is off.
    static void RegisterRowIcon(UIElement* icon);
    // A search field whose empty "Search" hint follows ShowIconAndHint. The registry keeps a weak
    // reference, so a destroyed field needs no unregister.
    static void RegisterHintTarget(UIElement* searchField);
    static void UnregisterRowIcon(UIElement* icon);
    
    // Load visibility setting from preferences
    static void LoadFromPreferences();

private:
    struct RegisteredBar
    {
        UIElement* SearchBar = nullptr;
        UIElement* PlacementTarget = nullptr;
    };

    static void ApplyPlacement(UIElement* placementTarget);
    static void ApplyFocusOutline(UIElement* searchBar);
    static void ApplyLook(UIElement* searchBar);
    static void ApplyHint(UIElement* searchBar);
    static void ApplyRowIcon(UIElement* icon);
    static void PruneNulls();
    static void SaveToPreferences();

    static inline bool s_Visible = false;
    static inline bool s_AtTop = false;
    static inline bool s_AccentFocusOutline = true;
    static inline bool s_ClassicLook = false;
    static inline bool s_ShowIconAndHint = false;
    static inline bool s_Loaded = false;
    static inline std::vector<RegisteredBar> s_Bars;
    static inline std::vector<UIElement*> s_FocusTargets;
    static inline std::vector<UIElement*> s_RowIcons;
};

} // namespace GameEngine
