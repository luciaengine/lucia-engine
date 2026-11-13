#pragma once

#include "Mathematics/Rect.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <optional>
#include <string>

namespace GameEngine::UI::Layout
{

namespace Detail
{

inline bool TryReadPx(const StyleDeclaration& overrides, const StyleProp<StyleLength>& prop, float& outValue)
{
    const std::optional<StyleLength> value = overrides.Get(prop);
    if (!value.has_value() || !value->IsPx())
        return false;
    outValue = value->Value;
    return true;
}

} // namespace Detail

/// The element's resolved horizontal padding and border, in logical px: what a control is
/// wider than its content box.
inline float HorizontalInsetPx(const UIElement& element)
{
    const auto& layout = element.GetResolvedStyle().Layout;
    return layout.Padding.Left + layout.Padding.Right + layout.BorderWidth.Left + layout.BorderWidth.Right;
}

inline bool TryGetAbsolutePosition(const UIElement& element, Mathematics::Rect& outRect)
{
    const std::optional<PositionType> position = element.Style().Get(Style::Position);
    if (!position.has_value() || *position != PositionType::Absolute)
        return false;

    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    if (!Detail::TryReadPx(element.Style(), Style::PositionLeft, left) ||
        !Detail::TryReadPx(element.Style(), Style::PositionTop, top) ||
        !Detail::TryReadPx(element.Style(), Style::Width, width) ||
        !Detail::TryReadPx(element.Style(), Style::Height, height))
    {
        return false;
    }

    outRect = Mathematics::Rect{left, top, width, height};
    return true;
}

// positionOnlyFastPath: pure moves skip layout dirt entirely, relying on
// ApplyLayoutOverrideRects to patch committed rects post-solve every frame.
// Only virtualization-managed movers (recycled rows/cells repositioned per
// scroll tick) may pass true: they are guaranteed px-sized + absolute (so the
// override-rect registry patches them) and their binds raise their own style
// dirt. Everything else (popups, headers, outlines) keeps layout dirt so the
// Yoga style stays in sync and hit-testing never sees a stale rect between
// the solve and FinalizeSolve.
inline bool SetAbsolutePosition(UIElement& element, const Mathematics::Rect& rect,
                                bool positionOnlyFastPath = false)
{
    const Mathematics::Rect clamped{
        rect.X,
        rect.Y,
        std::max(0.0f, rect.Width),
        std::max(0.0f, rect.Height),
    };

    Mathematics::Rect current{};
    const bool hadRect = TryGetAbsolutePosition(element, current);
    if (hadRect && current == clamped)
        return false;
    const bool sizeChanged = !hadRect || current.Width != clamped.Width || current.Height != clamped.Height;

    if (positionOnlyFastPath && !sizeChanged)
    {
        element.Style().SetPositionOnlyPx(clamped.X, clamped.Y);
        element.MarkDirty(UIElement::VisualDirty);
        return true;
    }

    element.Style()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(clamped.X))
        .Set(Style::PositionTop, StyleLength::Px(clamped.Y))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(clamped.Width))
        .Set(Style::Height, StyleLength::Px(clamped.Height));

    element.MarkDirty(UIElement::VisualDirty);
    return true;
}

inline bool ClearAbsolutePosition(UIElement& element)
{
    const bool hasAbsoluteKeys =
        element.Style().Has(Style::Position.id) ||
        element.Style().Has(Style::PositionLeft.id) ||
        element.Style().Has(Style::PositionTop.id) ||
        element.Style().Has(Style::PositionRight.id) ||
        element.Style().Has(Style::PositionBottom.id) ||
        element.Style().Has(Style::Width.id) ||
        element.Style().Has(Style::Height.id);
    if (!hasAbsoluteKeys)
        return false;

    element.Style().Reset(Style::Position);
    element.Style().Reset(Style::PositionLeft);
    element.Style().Reset(Style::PositionTop);
    element.Style().Reset(Style::PositionRight);
    element.Style().Reset(Style::PositionBottom);
    element.Style().Reset(Style::Width);
    element.Style().Reset(Style::Height);
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetElementInvisible(UIElement& element, bool invisible)
{
    if (invisible)
    {
        const std::optional<bool> visibility = element.Style().Get(Style::Visibility);
        const std::optional<bool> pointerEvents = element.Style().Get(Style::PointerEvents);
        if (visibility.has_value() && !visibility.value() && pointerEvents.has_value() && !pointerEvents.value())
            return false;

        element.Style().Set(Style::Visibility, false);
        element.Style().Set(Style::PointerEvents, false);
    }
    else
    {
        const bool hadVisibilityOverride = element.Style().Has(Style::Visibility.id);
        const bool hadPointerEventsOverride = element.Style().Has(Style::PointerEvents.id);
        if (!hadVisibilityOverride && !hadPointerEventsOverride)
            return false;

        element.Style().Reset(Style::Visibility);
        element.Style().Reset(Style::PointerEvents);
    }

    // `visibility` inherits through the cascade; without `StyleDirty`, descendants keep a
    // stale `visible` flag while the row suppresses its own primitives — children still draw.
    element.MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetElementHidden(UIElement& element, bool hidden)
{
    if (hidden)
    {
        const std::optional<DisplayMode> currentDisplay = element.Style().Get(Style::Display);
        if (currentDisplay.has_value() && currentDisplay.value() == DisplayMode::None)
            return false;

        element.Style().Set(Style::Display, DisplayMode::None);
    }
    else
    {
        if (!element.Style().Has(Style::Display.id))
            return false;
        element.Style().Reset(Style::Display);
    }

    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool SetForcedHeight(UIElement& element, int heightPx)
{
    const float clampedHeight = static_cast<float>(std::max(0, heightPx));
    const std::optional<StyleLength> currentHeight = element.Style().Get(Style::Height);
    if (currentHeight.has_value() && currentHeight->IsPx() && currentHeight->Value == clampedHeight)
        return false;

    element.Style().Set(Style::Height, StyleLength::Px(clampedHeight));
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline bool ClearForcedHeight(UIElement& element)
{
    if (!element.Style().Has(Style::Height.id))
        return false;

    Mathematics::Rect absoluteRect{};
    if (!TryGetAbsolutePosition(element, absoluteRect))
        element.Style().Reset(Style::Height);
    element.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

inline void SetBackgroundPath(UIElement& element, const std::string& path)
{
    BackgroundImageSource source{};
    source.Kind = BackgroundImageSource::SourceKind::Path;
    source.Value = path;
    element.Style()
        .Set(Style::BackgroundImage, source)
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue::Percent(50.0f, 50.0f))
        .Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

inline void SetBackgroundResourceName(UIElement& element, const std::string& resourceName)
{
    element.Style()
        .Set(Style::BackgroundImage, BackgroundImageSource::Resource(resourceName))
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue::Percent(50.0f, 50.0f))
        .Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

inline void ClearBackgroundOverride(UIElement& element)
{
    element.Style()
        .Set(Style::BackgroundImage, BackgroundImageSource::Resource({}))
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundPosition, BackgroundPositionValue::Percent(50.0f, 50.0f))
        .Set(Style::BackgroundTint, 0x00000000u);
    element.MarkDirty(UIElement::VisualDirty);
}

// Shows the texture registered as `resourceName` (a render-graph output) as a centered cover background.
// A viewport calls this on every frame, so it writes nothing while the element shows that texture: a
// write dirties the element and, through the sibling-combinator invalidation, the whole tree.
inline void BindViewportTexture(UIElement& element, const std::string& resourceName)
{
    const auto current = element.Style().Get(Style::BackgroundImage);
    if (current.has_value() && current->Kind == BackgroundImageSource::SourceKind::ResourceName &&
        current->Value == resourceName)
        return;
    // The reset before the set also requests a cascade re-run for the rebind.
    element.Style()
        .Reset(Style::BackgroundImage)
        .Set(Style::BackgroundImage, BackgroundImageSource::Resource(resourceName))
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Cover})
        .Set(Style::BackgroundPosition, BackgroundPositionValue::Percent(50.0f, 50.0f))
        .Reset(Style::BackgroundTint);
}

inline void DisableBackgroundOverride(UIElement& element)
{
    element.Style().Reset(Style::BackgroundImage);
    element.Style().Reset(Style::BackgroundRepeatProp);
    element.Style().Reset(Style::BackgroundSize);
    element.Style().Reset(Style::BackgroundPosition);
    element.Style().Reset(Style::BackgroundTint);
    element.MarkDirty(UIElement::VisualDirty);
}

} // namespace GameEngine::UI::Layout

