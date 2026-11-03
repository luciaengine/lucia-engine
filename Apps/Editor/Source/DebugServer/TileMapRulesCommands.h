#pragma once

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::TileMapECS { class TileMapRuntimeStore; }
namespace GameEngine::Editor
{
class TileMapEditContext;

/// Author rules through the mutable map store and publish its normal dirty ledger.
nlohmann::json AuthorTileMapRules(TileMapEditContext* editContext,
    TileMapECS::TileMapRuntimeStore* store, const nlohmann::json& params);
/// Describe a map's rules and an optional bounded cell window.
nlohmann::json DescribeTileMapRules(const TileMapEditContext* editContext,
    TileMapECS::TileMapRuntimeStore* store, const nlohmann::json& params);
} // namespace GameEngine::Editor
