#pragma once

#include "AssetCore/AssetTypes.h"
#include "Graph/GraphModel.h"
#include "Graph/NodeParamOption.h"

#include <string>
#include <vector>

namespace GameEngine
{

struct GameLogicActionParameter
{
    std::string Id;
    std::string DefaultValue;
    /** Non-empty = enum: every editor surface offers exactly these choices,
        matching the value vocabulary the runtime accepts. */
    Graph::NodeParamOptions Options;
    /** The type of asset whose GUID the parameter holds; Unknown when it holds no asset
        reference. Editors show an asset reference field filtered to this type. */
    AssetType AssetReference = AssetType::Unknown;
};

struct GameLogicActionSpec
{
    std::string TypeId;
    std::string DisplayName;
    std::string Category;
    std::vector<Graph::Port> Ports;
    std::vector<GameLogicActionParameter> Parameters;
    bool Precompiled = true;
};

const std::vector<GameLogicActionSpec>& GetGameLogicActionCatalog();
const GameLogicActionSpec* FindGameLogicActionSpec(const std::string& typeId);

} // namespace GameEngine
