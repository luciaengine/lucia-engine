#pragma once

#include "Types/Types.h"
#include <limits>
#include <nlohmann/json.hpp>

namespace GameEngine::Editor::TileMapCommands
{
// Do not narrow before checking: a large or negative JSON integer can alias a
// live layer or tile id after conversion. Floats, booleans and strings are not ids.
inline bool ReadUInt32(const nlohmann::json& value, uint32& out)
{
    if (!value.is_number_integer())
        return false;
    if (value.is_number_unsigned())
    {
        const uint64 number = value.get<uint64>();
        if (number > std::numeric_limits<uint32>::max())
            return false;
        out = static_cast<uint32>(number);
        return true;
    }
    const int64 number = value.get<int64>();
    if (number < 0 || number > static_cast<int64>(std::numeric_limits<uint32>::max()))
        return false;
    out = static_cast<uint32>(number);
    return true;
}
} // namespace GameEngine::Editor::TileMapCommands
