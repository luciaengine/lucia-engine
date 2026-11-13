#pragma once

#include "UI/UIElement.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class INativeToolbar;
class Label;
struct TitleBarMenuRegion;
namespace Platform { class Window; }
namespace Rendering::Text { class FontAtlas; }

/// Editor-owned caption contents. Native menus and frame behavior stay in Platform.
class EditorTitleBar final : public UIElement
{
  public:
    EditorTitleBar();
    ~EditorTitleBar() override;
    void Bind(INativeToolbar* toolbar, Platform::Window* window);
    /// Synchronizes the caption row; `caption` is the centered title text.
    void UpdateState(const std::string& caption);
    void OnPostLayout() override;

  private:
    struct MenuHeading
    {
        WeakRef<Button> Control;
        uint32_t Id = 0;
        Rendering::Text::FontAtlas* Font = nullptr;
        float PixelSize = 0.0f;
        float LetterSpacing = 0.0f;
        float MnemonicWidth = 0.0f;
    };

    bool LoadAssets();
    void RebuildMenus();
    void UpdateMnemonicLayout(MenuHeading& heading);
    void OpenMenu(uint32_t menuId);
    void PushTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions);

    INativeToolbar* m_Toolbar = nullptr;
    Platform::Window* m_Window = nullptr;
    uint64_t m_MenuRevision = UINT64_MAX;
    bool m_AssetsLoaded = false;
    bool m_Collapsed = false;
    std::vector<MenuHeading> m_MenuHeadings;
    WeakRef<UIElement> m_Icon;
    WeakRef<Label> m_Title;
    WeakRef<UIElement> m_MenusHost;
    WeakRef<Button> m_Overflow;
    // Minimize, maximize or restore, and close, in the order of TitleBarMetrics::Buttons.
    std::array<WeakRef<Label>, 3> m_CaptionLabels;
    // The layout last sent to the toolbar; -1 until the first send.
    int m_PushedHeight = -1;
    std::vector<TitleBarMenuRegion> m_PushedRegions;
};
} // namespace GameEngine
