#pragma once

#include "AssetCore/Types.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace TileMap
{
namespace JsonWrite
{

using Json = nlohmann::json;

/**
 * A float32 written to JSON as the shortest decimal that still reads back as
 * the same float32.
 *
 * nlohmann's number type is double, so assigning a float32 widens it and the
 * dump prints the widened value in full: 0.04f becomes 0.03999999910593033.
 * Those digits are the float's representation error spelled out, not accuracy —
 * they round-trip to the identical float32 either way, and they dominate a
 * file that writes several per tile. Every float32 an asset writer emits goes
 * through here.
 */
inline Json ShortestFloat(float32 value)
{
    // Leave non-finite values to nlohmann, which has its own representation for
    // them; only finite numbers are being shortened here.
    if (!std::isfinite(value))
        return Json(static_cast<float64>(value));

    char text[32];
    for (int32 digits = 1; digits <= 9; ++digits)
    {
        std::snprintf(text, sizeof(text), "%.*g", digits, static_cast<float64>(value));
        if (static_cast<float32>(std::strtod(text, nullptr)) == value)
            break;
    }
    return Json::parse(text);
}

} // namespace JsonWrite
} // namespace TileMap
} // namespace GameEngine
