#pragma once

#include "Editor/Settings/SettingsStore.h"

#include <cstdint>
#include <string>

namespace GameEngine
{
namespace Editor
{

/**
 * @brief User-level settings for the Tile Palette, persisted in the editor
 * preferences (Preferences.json) like TooltipSettings.
 */
class TilePaletteSettings
{
public:
    /** #202020, in 0xAARRGGBB like every colour setting. */
    static constexpr uint32_t kDefaultAtlasBackgroundArgb = 0xFF202020u;

    static TilePaletteSettings& Get()
    {
        static TilePaletteSettings instance;
        return instance;
    }

    void Load()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        int64_t background = 0;
        if (prefs.TryGetInt64("tilePalette.atlasBackground", background))
            m_AtlasBackgroundArgb = static_cast<uint32_t>(background);
    }

    void Save()
    {
        auto prefs = OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.SetInt64("tilePalette.atlasBackground", static_cast<int64_t>(m_AtlasBackgroundArgb));
        prefs.Save(&err);
    }

    /** The colour behind the atlas grid, 0xAARRGGBB. */
    uint32_t GetAtlasBackgroundArgb() const { return m_AtlasBackgroundArgb; }
    void SetAtlasBackgroundArgb(uint32_t argb)
    {
        m_AtlasBackgroundArgb = argb;
        Save();
    }

private:
    TilePaletteSettings() { Load(); }

    uint32_t m_AtlasBackgroundArgb = kDefaultAtlasBackgroundArgb;
};

} // namespace Editor
} // namespace GameEngine
