#pragma once

#include "AssetCore/Types.h"

#include <nlohmann/json.hpp>

#include <string>
#include <cmath>
#include <limits>

namespace GameEngine
{
namespace TileMap
{
namespace JsonRead
{

using Json = nlohmann::json;

/**
 * Type-checked reads for asset JSON.
 *
 * A member of the wrong type reads as the caller's fallback instead of
 * throwing. The asset parsers are linked into builds whose translation units
 * disagree about RTTI, where `catch (const std::exception&)` does not fire, so
 * no parser may depend on an exception escaping to report malformed input.
 * Parse the document with exceptions disabled and reject a discarded result to
 * complete the guarantee.
 */

/** The member `key`, or nullptr when absent (or `object` is not an object). */
inline const Json* Member(const Json& object, const char* key)
{
    if (!object.is_object())
        return nullptr;
    const auto it = object.find(key);
    return it != object.end() ? &(*it) : nullptr;
}

/** Integer member widened to int64; `fallback` when absent or not an integer. */
inline int64 Integer(const Json& object, const char* key, int64 fallback)
{
    const Json* member = Member(object, key);
    if (!member || !member->is_number_integer())
        return fallback;
    return member->is_number_unsigned() && member->get<uint64>() > uint64(INT64_MAX)
               ? fallback
               : member->get<int64>();
}

/** Number member (integers accepted); `fallback` when absent or not a number. */
inline float64 Number(const Json& object, const char* key, float64 fallback)
{
    const Json* member = Member(object, key);
    return member && member->is_number() ? member->get<float64>() : fallback;
}

/** Number element of an array (integers accepted); `fallback` when not a number. */
inline float64 NumberAt(const Json& array, size_t index, float64 fallback)
{
    if (!array.is_array() || index >= array.size() || !array[index].is_number())
        return fallback;
    return array[index].get<float64>();
}

inline bool FitsFloat32(float64 value)
{
    return std::isfinite(value) && value >= -std::numeric_limits<float32>::max() &&
           value <= std::numeric_limits<float32>::max();
}

inline int64 IntegerAt(const Json& array, size_t index, int64 fallback)
{
    if (!array.is_array() || index >= array.size() || !array[index].is_number_integer())
        return fallback;
    const auto& value = array[index];
    return value.is_number_unsigned() && value.get<uint64>() > uint64(INT64_MAX)
        ? fallback : value.get<int64>();
}

inline bool Boolean(const Json& object, const char* key, bool fallback)
{
    const Json* member = Member(object, key);
    return member && member->is_boolean() ? member->get<bool>() : fallback;
}

/**
 * String member with embedded NULs removed; `fallback` when absent or not a
 * string. A NUL truncates every C-string consumer downstream (window titles,
 * file names), so it never reaches the asset model.
 */
inline std::string Text(const Json& object, const char* key, std::string fallback = {})
{
    const Json* member = Member(object, key);
    if (!member || !member->is_string())
        return fallback;
    std::string text = member->get<std::string>();
    std::erase(text, '\0');
    return text;
}

/** Array member, or nullptr when absent or not an array. */
inline const Json* Array(const Json& object, const char* key)
{
    const Json* member = Member(object, key);
    return member && member->is_array() ? member : nullptr;
}

/** Object member, or nullptr when absent or not an object. */
inline const Json* Object(const Json& object, const char* key)
{
    const Json* member = Member(object, key);
    return member && member->is_object() ? member : nullptr;
}

} // namespace JsonRead
} // namespace TileMap
} // namespace GameEngine
