#pragma once

#include <string>
#include <memory>
#include "Localization/LocalizationCatalog.h"
#include "UI/ITextMeasurable.h"
#include "UI/UIElement.h"
namespace GameEngine {

class Label : public UIElement, public ITextMeasurable {
public:
    Label() { m_TextMeasurable = this; }

    // Resolve a stable message key using the owning UIManager's catalog.
    // fallback is literal text shown without a catalog or when resolution fails.
    void BindLocalization(StringId key, std::string fallback,
                          Localization::MessageArguments arguments = {});
    // Replaces named ICU placeholder values (e.g. {"count", int64_t{2}}).
    // Call when application values change; locale/catalog changes refresh automatically.
    void SetLocalizationArguments(Localization::MessageArguments arguments);

    /// Remove the localization binding while keeping the currently displayed text.
    void ClearLocalization();

    // Text API specific to Label
    const std::string& GetText() const { return m_Text; }
    /// Set literal text and remove any localization binding, even if the text is unchanged.
    void SetText(const std::string& t);
    /// Apply UXML text before the final authored hook, preserving localization arguments.
    void SetAuthoredText(const std::string& text);
    // For frequently-updated debug/perf overlays where the label's Yoga layout box
    // is known to be stable (e.g. fixed width / absolute positioned), allow a
    // paint-only update that avoids triggering a Yoga relayout.
	
		    // Provide intrinsic text measurement so UIManager/Yoga can size labels
		    // without knowing about Label specifically.
		    void GetTextMeasureInfo(ITextMeasurable::TextMeasureInfo& info,
		                           const ResolvedStyle& style) const override
	    {
	        (void)style;
	        info.HasText = !m_Text.empty();
	        info.Text = m_Text;
	        info.ExplicitFontSize = 0.0f; // use style.fontSize
	    }

	    bool AllowWrapForMeasure() const override { return true; }

	    // Expose label text to generic UIManager helpers (geometry overrides,
	    // debug overlays) without requiring it to know about Label.
	    const std::string& GetTextContent() const override { return m_Text; }

private:
    friend class UIManager;
    // UXML parse/reconcile hook: map localization-key and literal text to the same binding API.
    void OnAuthoredAttributesApplied(const std::unordered_map<std::string, std::string>& attributes) override;
    void OnOwnerManagerChanged(UIManager* owner) override;
    void RefreshLocalizedText();
    void ShowText(const std::string& text);
    StringId m_LocalizationKey = 0;
    std::string m_LocalizationFallback;
    Localization::MessageArguments m_LocalizationArguments;
    std::string m_Text;
};

} // namespace GameEngine

