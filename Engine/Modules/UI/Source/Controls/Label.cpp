#include "UI/Controls/Label.h"
#include "UI/UIManager.h"
#include "UI/Controls/ScrollView.h"
#include "UI/ResolvedStyle.h"
#include "Logger/Logger.h"

namespace GameEngine
{

void Label::OnAuthoredAttributesApplied(const std::unordered_map<std::string, std::string>& attributes)
{
    const auto key = attributes.find("localization-key");
    const auto text = attributes.find("text");
    std::string literal = text != attributes.end() ? text->second : GetText();
    if (key != attributes.end() && !key->second.empty())
    {
        BindLocalization(HashStringId(key->second), std::move(literal), m_LocalizationArguments);
        return;
    }
    ClearLocalization();
    ShowText(literal);
}

void Label::OnOwnerManagerChanged(UIManager*)
{
    RefreshLocalizedText();
}

void Label::BindLocalization(StringId key, std::string fallback, Localization::MessageArguments arguments)
{
    if (!key)
    {
        Logger::Log::Warning("UI localization: BindLocalization needs a nonzero key; showing \"{}\" unbound. "
                             "Call ClearLocalization to remove a binding.", fallback);
        ClearLocalization();
        ShowText(fallback);
        return;
    }
    m_LocalizationKey = key;
    m_LocalizationFallback = std::move(fallback);
    m_LocalizationArguments = std::move(arguments);
    RefreshLocalizedText();
}

void Label::ClearLocalization()
{
    m_LocalizationKey = 0;
    m_LocalizationFallback.clear();
    m_LocalizationArguments.clear();
}

void Label::RefreshLocalizedText()
{
    if (!m_LocalizationKey) return;
    auto* owner = GetOwnerManager();
    if (!owner || !owner->GetLocalizationCatalog())
    {
        ShowText(m_LocalizationFallback);
        return;
    }
    std::string error;
    ShowText(owner->GetLocalizationCatalog()->Resolve(m_LocalizationKey, m_LocalizationFallback,
                                                     m_LocalizationArguments, &error));
    if (!error.empty()) Logger::Log::Warning("UI localization: {}", error);
}

void Label::SetLocalizationArguments(Localization::MessageArguments arguments)
{
    if (m_LocalizationArguments == arguments) return;
    m_LocalizationArguments = std::move(arguments);
    RefreshLocalizedText();
}

void Label::SetText(const std::string& t)
{
    ClearLocalization();
    ShowText(t);
}

void Label::SetAuthoredText(const std::string& text)
{
    ShowText(text);
}

void Label::ShowText(const std::string& t)
{
    if (m_Text == t)
        return;
    m_Text = t;

    // Impact inference (callers do not declare the impact; it is derived
    // here): when the resolved style pins BOTH axes to definite px sizes,
    // Yoga takes the measure-free fast path and never consults the text
    // measure, so new text can only
    // change this label's own primitive bytes — content-only, handled by
    // the render-side drain with no layout, resolve, or idle-gate impact.
    // Any content-driven axis re-measures, so it takes the layout path.
    // The rect>0 guard covers the not-yet-laid-out first set.
    const ResolvedStyle& rs = GetResolvedStyle();
    const bool fixedSize = rs.Layout.Width.IsPx() && rs.Layout.Height.IsPx();
    if (fixedSize && GetLayoutWidth() > 0.0f && GetLayoutHeight() > 0.0f)
    {
        MarkContentDirty();
        return;
    }

    for (UIElement* p = GetParent(); p; p = p->GetParent())
    {
        if (auto* sv = dynamic_cast<ScrollView*>(p))
        {
            // Text changes can affect horizontal overflow measurement.
            sv->MarkHorizontalMeasureDirty();
        }
    }

    // Marking LayoutDirty is sufficient: UIManager's layout-signature pass will detect whether this
    // change actually affects Yoga layout (e.g. intrinsic text measurement) and only then run a solve.
    // Avoid RequestRelayout() here; it forces a conservative "signatures for all nodes" pass next frame.
    MarkDirty(LayoutDirty | VisualDirty);
}

} // namespace GameEngine
