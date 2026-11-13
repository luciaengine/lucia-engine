#include "UI/StyleDeclaration.h"

#include "UI/StyleProperties.h"

#include <sstream>

namespace GameEngine
{

void StyleDeclaration::SetImpl(StylePropertyId id, StyleDeclarationEntry entry, StyleImpact impact)
{
    // Setting an override to its current value is a no-op. Rebind-heavy paths
    // (virtualized rows re-applying position/size/padding on every bind) would
    // otherwise mark layout dirt and cascade re-runs for values that did not
    // change, forcing Yoga rebuild+solve frames for pure recycling.
    if (StyleDeclarationEntry* existing = m_Props.Find(id))
    {
        if (existing->Keyword == entry.Keyword && existing->Value == entry.Value)
            return;
        *existing = std::move(entry);
    }
    else
    {
        m_Props.InsertOrAssign(id, std::move(entry));
    }
    if (impact == StyleImpact::Layout)
    {
        m_LayoutDirty = true;
        NotifyDirty(Layout);
        return;
    }

    m_VisualDirty = true;
    // A paint-impact property whose effect reaches descendants
    // (StylePropertyImpact::Subtree) needs more than the per-element drain a
    // bare Visual mark gets: the drain rewrites this element's primitive bytes
    // and never revisits a child. Cascade is the bit UIElement maps to
    // StyleDirty, which is what NotifyDirty_UpdateRegenFlag escalates to a full
    // regen — and the regen is required rather than merely convenient, because
    // a drained descendant restores its RECORDED ancestor opacity
    // (UIManager_PrimitiveGen.cpp) and re-uses clip slots only the DFS assigns.
    const uint32_t bits = GetStylePropertyImpact(id).Subtree ? (Visual | Cascade) : Visual;
    NotifyDirty(bits);
}

void StyleDeclaration::SetPositionOnlyPx(float left, float top)
{
    SetImpl(Style::PositionLeft.id,
            StyleDeclarationEntry{StyleKeyword::None, StyleValue{StyleLength::Px(left)}},
            StyleImpact::Visual);
    SetImpl(Style::PositionTop.id,
            StyleDeclarationEntry{StyleKeyword::None, StyleValue{StyleLength::Px(top)}},
            StyleImpact::Visual);
}

void StyleDeclaration::SetCustom(StringId name, std::string value)
{
    if (std::string* existing = m_CustomVars.Find(name))
    {
        if (*existing == value)
            return;
        *existing = std::move(value);
    }
    else
    {
        m_CustomVars.InsertOrAssign(name, std::move(value));
    }
    m_VisualDirty = true;
    /* The cascade resolves a custom property, and any descendant can read it
       through var(). Thus this element and every element that inherits from it
       must cascade again. */
    m_NeedsCascadeRerun = true;
    NotifyDirty(Visual | Cascade | CustomScope);
}

void StyleDeclaration::SetCustomNumber(StringId name, float value)
{
    std::ostringstream ss;
    ss << value;
    SetCustom(name, ss.str());
}

void StyleDeclaration::SetCustomColor(StringId name, uint32_t argb)
{
    // CSS 8-digit hex is #RRGGBBAA (see CSSValueParsers/ResolvedStyle), so the
    // alpha moves from the ARGB high byte to the tail.
    char buf[12];
    std::snprintf(buf, sizeof(buf), "#%06X%02X", argb & 0xFFFFFFu, (argb >> 24) & 0xFFu);
    SetCustom(name, std::string(buf));
}

void StyleDeclaration::ClearCustom(StringId name)
{
    if (m_CustomVars.Erase(name))
    {
        m_VisualDirty = true;
        m_NeedsCascadeRerun = true;
        NotifyDirty(Visual | Cascade | CustomScope);
    }
}

const std::string* StyleDeclaration::FindCustom(StringId name) const
{
    return m_CustomVars.Find(name);
}

void StyleDeclaration::SetById(StylePropertyId id, const StyleValue& value, StyleImpact impact)
{
    SetImpl(id, StyleDeclarationEntry{StyleKeyword::None, value}, impact);
}

void StyleDeclaration::SetKeywordById(StylePropertyId id, StyleKeyword kw, StyleImpact impact)
{
    SetImpl(id, StyleDeclarationEntry{kw, {}}, impact);
}

} // namespace GameEngine
