#include "DebugServer/TileMapRulesWindow.h"

#include "TileMap/TileCellEncoding.h"

#include <limits>

namespace GameEngine::Editor
{
bool BuildTileMapRulesWindow(const TileMap::TileMapLayerData& layer,
                            const nlohmann::json& bounds, nlohmann::json& result)
{
    if (!bounds.is_object())
        return false;
    const auto coordinate = [&bounds](const char* key, int64& value)
    {
        value = 0;
        const auto it = bounds.find(key);
        if (it == bounds.end())
            return true;
        if (!it->is_number_integer())
            return false;
        if (it->is_number_unsigned())
        {
            const uint64 number = it->get<uint64>();
            if (number > static_cast<uint64>(std::numeric_limits<int32>::max()))
                return false;
            value = static_cast<int64>(number);
        }
        else
            value = it->get<int64>();
        return value >= std::numeric_limits<int32>::min() && value <= std::numeric_limits<int32>::max();
    };
    int64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!coordinate("x0", x0) || !coordinate("y0", y0) ||
        !coordinate("x1", x1) || !coordinate("y1", y1))
        return false;
    const int64 width = x1 - x0 + 1;
    const int64 height = y1 - y0 + 1;
    constexpr int64 kMaxWindowCells = 4096;
    if (width <= 0 || height <= 0 || width > kMaxWindowCells || height > kMaxWindowCells ||
        width > kMaxWindowCells / height)
        return false;

    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json intRows = nlohmann::json::array();
    nlohmann::json autoRows = nlohmann::json::array();
    for (int64 y = y1; y >= y0; --y)
    {
        nlohmann::json row = nlohmann::json::array();
        nlohmann::json intRow = nlohmann::json::array();
        nlohmann::json autoRow = nlohmann::json::array();
        for (int64 x = x0; x <= x1; ++x)
        {
            const auto cellX = static_cast<int32>(x);
            const auto cellY = static_cast<int32>(y);
            row.push_back(TileMap::CellTileId(layer.GetCell(cellX, cellY)));
            intRow.push_back(layer.GetIntGridValue(cellX, cellY));
            autoRow.push_back(layer.IsAutoCell(cellX, cellY) ? 1 : 0);
        }
        rows.push_back(std::move(row));
        intRows.push_back(std::move(intRow));
        autoRows.push_back(std::move(autoRow));
    }
    result = {{"layer", layer.Id}, {"tileIds", std::move(rows)},
              {"intGrid", std::move(intRows)}, {"auto", std::move(autoRows)}};
    return true;
}
}
