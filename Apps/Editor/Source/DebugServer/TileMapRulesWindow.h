#pragma once

#include "TileMap/TileMapLayerData.h"
#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
/** Builds the bounded tilemap_rules_state window; refuses invalid coordinates or cell budgets. */
bool BuildTileMapRulesWindow(const TileMap::TileMapLayerData& layer,
                            const nlohmann::json& bounds, nlohmann::json& result);
}
