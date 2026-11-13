#pragma once

#include "Editor/Settings/SettingsStore.h"

namespace GameEngine
{
namespace Editor
{

// User-level settings for editor tooltips.
// Values are persisted in the Editor preferences (Preferences.json).
class TooltipSettings
{
public:
    static TooltipSettings& Get()
    {
        static TooltipSettings instance;
        return instance;
    }

    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        double delay = static_cast<double>(m_HoverDelaySeconds);
        prefs.TryGetDouble("tooltip.hoverDelaySeconds", delay);
        double resetDelay = static_cast<double>(m_HoverResetDelaySeconds);
        prefs.TryGetDouble("tooltip.hoverResetDelaySeconds", resetDelay);
        prefs.TryGetBool("tooltip.enabled", m_Enabled);
        prefs.TryGetBool("tooltip.middleMouseShow", m_MiddleMouseShow);

        m_HoverDelaySeconds = static_cast<float>(delay);
        m_HoverResetDelaySeconds = static_cast<float>(resetDelay);
        Validate();
    }

    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        prefs.SetDouble("tooltip.hoverDelaySeconds", static_cast<double>(m_HoverDelaySeconds));
        prefs.SetDouble("tooltip.hoverResetDelaySeconds", static_cast<double>(m_HoverResetDelaySeconds));
        prefs.SetBool("tooltip.enabled", m_Enabled);
        prefs.SetBool("tooltip.middleMouseShow", m_MiddleMouseShow);

        prefs.Save(&err);
    }

    float GetHoverDelaySeconds()      const { return m_HoverDelaySeconds; }
    float GetHoverResetDelaySeconds() const { return m_HoverResetDelaySeconds; }
    bool GetEnabled()                 const { return m_Enabled; }
    bool GetMiddleMouseShow()         const { return m_MiddleMouseShow; }

    void SetHoverDelaySeconds(float value)
    {
        m_HoverDelaySeconds = value;
        Validate();
        Save();
    }

    void SetHoverResetDelaySeconds(float value)
    {
        m_HoverResetDelaySeconds = value;
        Validate();
        Save();
    }

    void SetEnabled(bool value)
    {
        m_Enabled = value;
        Save();
    }

    void SetMiddleMouseShow(bool value)
    {
        m_MiddleMouseShow = value;
        Save();
    }

private:
    TooltipSettings() { Load(); }

    void Validate()
    {
        constexpr float kMinDelay = 0.0f;
        constexpr float kMaxDelay = 5.0f;
        constexpr float kDefaultHoverDelay = 0.5f;
        constexpr float kDefaultResetDelay = 0.5f;

        if (!(m_HoverDelaySeconds >= kMinDelay))
            m_HoverDelaySeconds = kDefaultHoverDelay;
        if (m_HoverDelaySeconds > kMaxDelay)
            m_HoverDelaySeconds = kMaxDelay;
        if (!(m_HoverResetDelaySeconds >= kMinDelay))
            m_HoverResetDelaySeconds = kDefaultResetDelay;
        if (m_HoverResetDelaySeconds > kMaxDelay)
            m_HoverResetDelaySeconds = kMaxDelay;
    }

    float    m_HoverDelaySeconds      = 0.5f;
    float    m_HoverResetDelaySeconds = 0.5f;
    bool     m_Enabled         = true;
    bool     m_MiddleMouseShow = false;
};

} // namespace Editor
} // namespace GameEngine
