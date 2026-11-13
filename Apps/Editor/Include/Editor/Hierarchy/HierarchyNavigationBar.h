#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
namespace EditorUI
{
class ItemSizeSlider;
}

namespace Editor
{

// The Hierarchy's item-size slider and hosted search move together with the search placement
// setting. A rows glyph leads the slider, authored in HierarchyNavigationBar.uxml; the subtree
// uses the stylesheet beside that layout.
//
// The slider sets the Hierarchy's tree icon size. The bar reports sizes through two callbacks and
// is told the current size back; the panel owns the size.
class HierarchyNavigationBar : public UIElement
{
public:
    HierarchyNavigationBar();

    // The size the slider shows. Raises neither callback, and leaves the slider alone while it is
    // reporting a drag, so the drag keeps its thumb.
    void SetItemSize(float px);

    // While the slider drags: the size under the thumb.
    void SetOnItemSizeChanging(std::function<void(float)> callback) { m_OnItemSizeChanging = std::move(callback); }

    // A size to commit: the slider released, or the item resize gesture over the slider.
    void SetOnItemSizeChanged(std::function<void(float)> callback) { m_OnItemSizeChanged = std::move(callback); }

    // Owns the panel search as a flexible item beside the size slider.
    void SetSearchBar(std::unique_ptr<UIElement> searchBar);

    // Lifts the row over the tree's horizontal scrollbar while one is on screen.
    void SetAboveHorizontalScrollbar(bool above);

private:
    void AddSizeGlyph(const std::string& tooltip);
    void ReportDrag(float px);
    void ApplyResizeGesture(float scrollY);

    EditorUI::ItemSizeSlider* m_SizeSlider = nullptr;
    std::function<void(float)> m_OnItemSizeChanging;
    std::function<void(float)> m_OnItemSizeChanged;
    bool m_ReportingDrag = false;
};

} // namespace Editor
} // namespace GameEngine
