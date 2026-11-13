#pragma once

#include "Types/FlatMap.h"
#include "Types/StringId.h"
#include "UI/StyleProp.h"
#include "UI/UIStyle.h"

#include <optional>
#include <string>
#include <variant>

namespace GameEngine
{

struct StyleDeclarationEntry
{
    StyleKeyword Keyword = StyleKeyword::None;
    StyleValue Value{};
};

// A sparse set of style declarations keyed by typed property (a CSS declaration block). An element
// holds two: UIElement::Style() (set by code) and UIElement::InlineStyle() (the parsed `style`
// attribute). A FlatMap: O(log N) lookup in contiguous memory, usually fewer than 10 entries.
class StyleDeclaration
{
  public:
    // Typed set: compile-time type safety via StyleProp<T>. Impact comes from
    // the canonical classification via StyleProp::Impact().
    template <typename T>
    StyleDeclaration& Set(StyleProp<T> prop, const T& value)
    {
        SetImpl(prop.id, StyleDeclarationEntry{StyleKeyword::None, StyleValue{value}}, prop.Impact());
        return *this;
    }

    // Set a CSS-wide keyword (inherit, initial, unset)
    template <typename T>
    StyleDeclaration& SetKeyword(StyleProp<T> prop, StyleKeyword kw)
    {
        SetImpl(prop.id, StyleDeclarationEntry{kw, {}}, prop.Impact());
        return *this;
    }

    // Reset (remove) a property override. Signals that the CSS cascade must
    // re-run to recover the base value for the removed property.
    template <typename T>
    StyleDeclaration& Reset(StyleProp<T> prop)
    {
        if (m_Props.Erase(prop.id))
        {
            const bool layout = (prop.Impact() == StyleImpact::Layout);
            if (layout)
                m_LayoutDirty = true;
            else
                m_VisualDirty = true;
            m_NeedsCascadeRerun = true;
            NotifyDirty((layout ? Layout : Visual) | Cascade);
        }
        return *this;
    }

    // Typed get: returns std::optional<T>
    template <typename T>
    std::optional<T> Get(StyleProp<T> prop) const
    {
        if (const auto* entry = m_Props.Find(prop.id))
        {
            if (auto* val = std::get_if<T>(&entry->Value))
                return *val;
        }
        return std::nullopt;
    }

    // Check if a property is overridden
    bool Has(StylePropertyId id) const { return m_Props.Contains(id); }

    // Dirty tracking
    bool IsLayoutDirty() const { return m_LayoutDirty; }
    bool IsVisualDirty() const { return m_VisualDirty; }
    bool NeedsCascadeRerun() const { return m_NeedsCascadeRerun; }
    void ClearDirty()
    {
        m_LayoutDirty = false;
        m_VisualDirty = false;
        m_NeedsCascadeRerun = false;
    }
    bool IsEmpty() const { return m_Props.Empty() && m_CustomVars.Empty(); }

    // Position-only fast path for virtualized recycling: updates the
    // PositionLeft/PositionTop entries WITHOUT raising layout dirt or a
    // cascade re-run. Callers must ensure the element is already
    // position:absolute with an explicit px size — committed rects are
    // patched by ApplyLayoutOverrideRects post-solve and primitives follow
    // via the drain, so Yoga participation is unnecessary for a pure move.
    void SetPositionOnlyPx(float left, float top);

    // Custom properties (CSS variables)
    void SetCustom(StringId name, std::string value);
    void SetCustomNumber(StringId name, float value);
    void SetCustomColor(StringId name, uint32_t argb);
    void ClearCustom(StringId name);
    const std::string* FindCustom(StringId name) const;

    // Iteration (for cascade resolver)
    template <typename Fn>
    void ForEachProp(Fn&& fn) const
    {
        m_Props.ForEach(std::forward<Fn>(fn));
    }
    template <typename Fn>
    void ForEachCustom(Fn&& fn) const
    {
        m_CustomVars.ForEach(std::forward<Fn>(fn));
    }

    size_t PropCount() const { return m_Props.Size(); }
    size_t CustomCount() const { return m_CustomVars.Size(); }

    void Clear()
    {
        const bool hadCustom = !m_CustomVars.Empty();
        const bool hadProps = !m_Props.Empty() || hadCustom;
        m_Props.Clear();
        m_CustomVars.Clear();
        // If we actually removed overrides, the cascade must re-run so the
        // element falls back to its CSS base values. Without this, layout
        // stays stale (e.g. an overlay with a cleared Display:Flex override
        // remains laid out as flex even though CSS says display:none).
        if (hadProps) { NotifyDirty(Layout | Visual | Cascade | (hadCustom ? CustomScope : 0u)); }
        if (hadProps)
        {
            m_LayoutDirty = true;
            m_VisualDirty = true;
            m_NeedsCascadeRerun = true;
        }
    }

    // Untyped set by property ID (for CSS parser integration where type is
    // already validated at parse time).
    void SetById(StylePropertyId id, const StyleValue& value, StyleImpact impact);
    void SetKeywordById(StylePropertyId id, StyleKeyword kw, StyleImpact impact);

    // Every mutator calls the dirty callback with these bits. UIElement sets the
    // callback at construction, so MarkDirty runs and marks the ancestors.
    // CustomScope: a custom property changed, so the subtree that inherits it re-cascades.
    enum DirtyBit : uint32_t
    {
        Layout      = 1u << 0,
        Visual      = 1u << 1,
        Cascade     = 1u << 2,
        CustomScope = 1u << 3,
    };
    using DirtyCallback = void(*)(void* ctx, uint32_t flags);
    void SetDirtyCallback(DirtyCallback cb, void* ctx)
    {
        m_DirtyCb = cb;
        m_DirtyCtx = ctx;
    }

  private:
    FlatMap<StylePropertyId, StyleDeclarationEntry> m_Props;
    FlatMap<StringId, std::string> m_CustomVars;

    bool m_LayoutDirty = false;
    bool m_VisualDirty = false;
    bool m_NeedsCascadeRerun = false;

    DirtyCallback m_DirtyCb = nullptr;
    void*         m_DirtyCtx = nullptr;

    void NotifyDirty(uint32_t flags)
    {
        if (m_DirtyCb)
            m_DirtyCb(m_DirtyCtx, flags);
    }

    void SetImpl(StylePropertyId id, StyleDeclarationEntry entry, StyleImpact impact);
};

} // namespace GameEngine
