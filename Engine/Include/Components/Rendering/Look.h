#pragma once

#include <cstdint>
#include <string_view>

namespace GameEngine {
namespace Components {

/// The display transform a PostProcessVolume renders its image with. Stored in a scene by name
/// (`PostProcessVolume.look = Filmic`), never by number, so reordering this enum cannot change
/// what a saved scene means. The values are tonemap.frag's `look` push constant.
enum class Look : int32_t
{
    Neutral = 0,      ///< Khronos PBR Neutral: colors drop by up to 4% of white, highlights soften; the default.
    Filmic = 1,       ///< ACES 2: film contrast, highlights roll off with little hue shift.
    Photographic = 2, ///< AgX: gentle contrast, highlights fade like film stock.
    GT7 = 3,          ///< ICtCp tone mapping with highlight desaturation.
};

inline constexpr int32_t kLookCount = 4;

/// The names used by the scene format and loader.
inline constexpr std::string_view kLookNames[kLookCount] = {"Neutral", "Filmic", "Photographic", "GT7"};

constexpr std::string_view LookName(Look look)
{
    return kLookNames[static_cast<int32_t>(look)];
}

/// What a look does to highlights and colour and when to pick it: the inspector's caption under
/// the Look list and the user guide's table read the same words.
constexpr std::string_view LookDescription(Look look)
{
    switch (look)
    {
    case Look::Neutral:
        return "Lowers every color by up to 4 percent of white, so shadows read slightly darker and dark colors "
               "slightly more saturated; only the brightest highlights soften to white. For product shots and "
               "stylized art.";
    case Look::Filmic:
        return "Film contrast: deeper shadows; bright colors roll off to white with little hue shift. For cinematic "
               "scenes.";
    case Look::Photographic:
        return "Gentle contrast; bright colors fade toward white like film, shifting slightly in hue. For natural "
               "daylight.";
    case Look::GT7:
        return "ICtCp tone mapping that preserves highlight color and fades bright highlights toward white.";
    }
    return {};
}

/// The tonemapper name shown in the editor.
constexpr std::string_view TonemapperName(Look look)
{
    switch (look)
    {
    case Look::Neutral:
        return "Khronos PBR Neutral";
    case Look::Filmic:
        return "ACES 2";
    case Look::Photographic:
        return "AgX";
    case Look::GT7:
        return "GT7 (ICtCp)";
    }
    return {};
}

} // namespace Components
} // namespace GameEngine
