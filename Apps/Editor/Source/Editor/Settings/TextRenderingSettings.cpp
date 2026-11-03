#include "Editor/Settings/TextRenderingSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <string>

namespace GameEngine::Editor
{

TextRenderingSettings& TextRenderingSettings::Get()
{
    static TextRenderingSettings instance;
    return instance;
}

TextRenderingSettings::TextRenderingSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    prefs.TryGetBool(kPrefKeySubpixelAA, m_SubpixelAA);
    double stored = m_Contrast;
    if (prefs.TryGetDouble(kPrefKeyContrast, stored))
        m_Contrast = std::clamp(static_cast<float>(stored), kMinContrast, kMaxContrast);
    stored = m_SmoothingGamma;
    if (prefs.TryGetDouble(kPrefKeySmoothingGamma, stored))
        m_SmoothingGamma = std::clamp(static_cast<float>(stored), kMinSmoothingGamma, kMaxSmoothingGamma);
}

void TextRenderingSettings::SetSubpixelAA(bool enabled)
{
    m_SubpixelAA = enabled;
    ApplyToTrackedWindows();
}

void TextRenderingSettings::SetContrast(float contrast)
{
    m_Contrast = contrast;
    ApplyToTrackedWindows();
}

void TextRenderingSettings::SetSmoothingGamma(float gamma)
{
    m_SmoothingGamma = gamma;
    ApplyToTrackedWindows();
}

void TextRenderingSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

void TextRenderingSettings::ApplyToWindow(UIManager& ui) const
{
    ui.SetTextSubpixelAA(m_SubpixelAA);
    ui.SetTextContrast(m_Contrast);
    ui.SetTextSmoothingGamma(m_SmoothingGamma);
}

void TextRenderingSettings::ApplyToTrackedWindows()
{
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

} // namespace GameEngine::Editor
