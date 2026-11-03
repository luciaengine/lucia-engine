#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <string_view>

namespace GameEngine::Components
{

struct GameLogicGraphRef
{
    static constexpr uint32 kEntryFunctionCapacity = 64;

    AssetRef<AssetType::Graph> graphGuid;
    bool runOnStart = true;
    bool runOnUpdate = true;
    char entryFunction[kEntryFunctionCapacity] = "ExecuteGameLogicGraph";

    std::string_view EntryFunction() const { return entryFunction; }

    void SetEntryFunction(std::string_view value)
    {
        CopyToFixedString(entryFunction, value.empty() ? std::string_view("ExecuteGameLogicGraph") : value);
    }
};

} // namespace GameEngine::Components
