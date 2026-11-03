#pragma once

#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/TrackedEditorWindows.h"

#include <algorithm>

namespace GameEngine
{
class UIManager;
}

namespace GameEngine::Editor
{

// Global script editor settings that control editor behavior.
// Settings are persisted in Editor preferences (Preferences.json).
class ScriptEditorSettings
{
public:
    static constexpr bool kDefaultOpenInScriptInspector = true;
    static constexpr bool kDefaultOpenShaderGraphGlslInMaterialGraph = true;
    static constexpr float kDefaultLineHeight = 1.0f;
    static constexpr float kMinLineHeight = 0.5f;
    static constexpr float kMaxLineHeight = 4.0f;
    static ScriptEditorSettings& Get()
    {
        static ScriptEditorSettings instance;
        return instance;
    }

    // Load settings from preferences file
    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        prefs.TryGetBool("script.openInScriptInspector", m_OpenInScriptInspector);
        prefs.TryGetBool("nativeScript.openInScriptInspector", m_OpenNativeSourceInScriptInspector);
        prefs.TryGetBool("script.openShaderGraphGlslInMaterialGraph", m_OpenShaderGraphGlslInMaterialGraph);

        double lineHeight = m_LineHeight;
        if (prefs.TryGetDouble("script.lineHeight", lineHeight))
            m_LineHeight = static_cast<float>(std::clamp(lineHeight, 0.5, 4.0));

        double textScalePercent = m_TextScalePercent;
        if (prefs.TryGetDouble("script.textScalePercent", textScalePercent))
            m_TextScalePercent = static_cast<float>(std::clamp(textScalePercent, 50.0, 200.0));
    }

    // Save settings to preferences file
    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err); // Load existing to preserve other settings

        prefs.SetBool("script.openInScriptInspector", m_OpenInScriptInspector);
        prefs.SetBool("nativeScript.openInScriptInspector", m_OpenNativeSourceInScriptInspector);
        prefs.SetBool("script.openShaderGraphGlslInMaterialGraph", m_OpenShaderGraphGlslInMaterialGraph);
        prefs.SetDouble("script.lineHeight", static_cast<double>(m_LineHeight));
        prefs.SetDouble("script.textScalePercent", static_cast<double>(m_TextScalePercent));
        prefs.Save(&err);
    }

    // When true (default), double-clicking a log line with file path opens in the internal Script Inspector.
    // When false, it opens in the system's default IDE.
    bool GetOpenInScriptInspector() const { return m_OpenInScriptInspector; }
    void SetOpenInScriptInspector(bool value)
    {
        m_OpenInScriptInspector = value;
        Save();
    }

    // When false (default), double-clicking a native C/C++ source opens it in the external IDE.
    // When true, it opens in the internal Script Editor panel. Independent of the C# setting above
    // (C++ developers generally want their IDE for native source).
    bool GetOpenNativeSourceInScriptInspector() const { return m_OpenNativeSourceInScriptInspector; }
    void SetOpenNativeSourceInScriptInspector(bool value)
    {
        m_OpenNativeSourceInScriptInspector = value;
        Save();
    }

    // When true (default), double-clicking a .glsl with @sg-graph tags opens the material graph editor.
    bool GetOpenShaderGraphGlslInMaterialGraph() const { return m_OpenShaderGraphGlslInMaterialGraph; }
    void SetOpenShaderGraphGlslInMaterialGraph(bool value)
    {
        m_OpenShaderGraphGlslInMaterialGraph = value;
        Save();
    }

    // Line height multiplier for the script editor textarea (default 1.0), in
    // multiples of the script face's own line. Setting it persists and restyles
    // every editor window the line height has been applied to.
    float GetLineHeight() const { return m_LineHeight; }
    void SetLineHeight(float value);

    // Push the script line height into `ui` and track it for later changes.
    // Call once per editor window, after its font preferences are applied.
    void ApplyTo(UIManager* ui);

    // Re-derive the line height in every tracked window: the multiplier is
    // relative to the script face, so a font change needs a new value.
    void ReapplyLineHeight();

    // Text size percentage for the main script editor (default 100%).
    float GetTextScalePercent() const { return m_TextScalePercent; }
    void SetTextScalePercent(float value)
    {
        m_TextScalePercent = std::clamp(value, 50.0f, 200.0f);
        Save();
    }

private:
    ScriptEditorSettings()
    {
        Load(); // Load on first access
    }
    ScriptEditorSettings(const ScriptEditorSettings&) = delete;
    ScriptEditorSettings& operator=(const ScriptEditorSettings&) = delete;

    void ApplyLineHeightToWindow(UIManager& ui) const;

    bool  m_OpenInScriptInspector = true;
    bool  m_OpenNativeSourceInScriptInspector = false; // native C/C++ defaults to the external IDE
    bool  m_OpenShaderGraphGlslInMaterialGraph = true;
    float m_LineHeight = 1.0f;
    float m_TextScalePercent = 100.0f;
    TrackedEditorWindows m_Windows;
};

} // namespace GameEngine::Editor
