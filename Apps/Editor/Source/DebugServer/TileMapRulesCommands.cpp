#include "DebugServer/TileMapRulesCommands.h"
#include "DebugServer/TileMapCommandNumbers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/TileMapRulesWindow.h"
#include "TileMap/TileMapEditContext.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "TileMap/RuleWizard.h"
#include "TileMap/TileRules.h"
#include "TileMap/TileSetAsset.h"
#include "TileMap/TileCellEncoding.h"

#include <cstdlib>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
using json = nlohmann::json;
namespace
{
/** Parses one pattern token: "." don't care, "*" any, "=N" require, "!N" forbid. */
bool ParsePatternToken(const std::string& token, TileMap::RulePatternCell& outCell)
{
    if (token == ".")
    {
        outCell = TileMap::RulePatternCell{};
        return true;
    }
    if (token == "*")
    {
        outCell = TileMap::RulePatternCell{TileMap::RulePatternOp::RequireAny, 0};
        return true;
    }
    if (token.size() < 2 || (token[0] != '=' && token[0] != '!'))
        return false;
    const int64 value = std::strtoll(token.c_str() + 1, nullptr, 10);
    if (value < 0 || value > 0xFF)
        return false;
    outCell = TileMap::RulePatternCell{token[0] == '=' ? TileMap::RulePatternOp::RequireValue
                                                       : TileMap::RulePatternOp::ForbidValue,
                                       static_cast<uint8>(value)};
    return true;
}

/** Splits a comma-separated pattern row into its tokens. */
std::vector<std::string> SplitPatternRow(const std::string& row)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (true)
    {
        const size_t comma = row.find(',', start);
        tokens.push_back(row.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos)
            return tokens;
        start = comma + 1;
    }
}

/** Builds one rule from its JSON spec; returns false with a reason on malformed input. */
bool ParseRuleSpec(const json& ruleJson, uint32 id, TileMap::TileRule& outRule, std::string& outError)
{
    outRule.Id = id;
    outRule.Name = ruleJson.value("name", std::string());
    outRule.Chance = ruleJson.value("chance", 1.0f);
    outRule.BreakOnMatch = ruleJson.value("breakOnMatch", true);
    outRule.MirrorX = ruleJson.value("mirrorX", false);
    outRule.MirrorY = ruleJson.value("mirrorY", false);
    outRule.ModuloX = ruleJson.value("moduloX", 1);
    outRule.ModuloY = ruleJson.value("moduloY", 1);
    outRule.OffsetX = ruleJson.value("offsetX", 0);
    outRule.OffsetY = ruleJson.value("offsetY", 0);

    if (!ruleJson.contains("pattern") || !ruleJson["pattern"].is_array())
    {
        outError = "each rule needs a pattern array of row strings";
        return false;
    }
    const int32 size = static_cast<int32>(ruleJson["pattern"].size());
    if (!TileMap::IsValidRulePatternSize(size))
    {
        outError = "pattern must have 1, 3 or 5 rows";
        return false;
    }
    outRule.PatternSize = size;
    outRule.Pattern.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                           TileMap::RulePatternCell{});
    for (int32 y = 0; y < size; ++y)
    {
        const std::vector<std::string> tokens =
            SplitPatternRow(ruleJson["pattern"][y].get<std::string>());
        if (static_cast<int32>(tokens.size()) != size)
        {
            outError = "every pattern row must have as many cells as there are rows";
            return false;
        }
        for (int32 x = 0; x < size; ++x)
        {
            if (!ParsePatternToken(tokens[static_cast<size_t>(x)],
                                   outRule.Pattern[static_cast<size_t>(y) *
                                                       static_cast<size_t>(size) +
                                                   static_cast<size_t>(x)]))
            {
                outError = "pattern tokens must be one of . * =N !N";
                return false;
            }
        }
    }

    if (ruleJson.contains("stamp"))
    {
        outRule.OutputMode = TileMap::RuleOutputMode::Stamp;
        outRule.Stamp.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                             TileMap::kEmptyCell);
        if (!ruleJson["stamp"].is_array() ||
            ruleJson["stamp"].size() != outRule.Stamp.size())
        {
            outError = "stamp must hold one tile id per pattern cell";
            return false;
        }
        for (size_t i = 0; i < outRule.Stamp.size(); ++i)
            outRule.Stamp[i] = TileMap::MakeCell(ruleJson["stamp"][i].get<uint32>());
        return true;
    }

    if (!ruleJson.contains("tiles") || !ruleJson["tiles"].is_array() ||
        ruleJson["tiles"].empty())
    {
        outError = "each rule needs a non-empty tiles array (or a stamp)";
        return false;
    }
    for (const auto& tileJson : ruleJson["tiles"])
    {
        const uint32 tileId =
            tileJson.is_object() ? tileJson.value("tileId", 0u) : tileJson.get<uint32>();
        const float32 weight = tileJson.is_object() ? tileJson.value("weight", 1.0f) : 1.0f;
        outRule.Tiles.push_back(TileMap::RuleTileOutput{TileMap::MakeCell(tileId), weight});
    }
    return true;
}

} // namespace

json AuthorTileMapRules(TileMapEditContext* editContext, TileMapECS::TileMapRuntimeStore* store, const json& params)
{
    if (!editContext || !store)
        return Editor::RefuseRequest("No tilemap edit context or runtime store");

    const GUID mapGuid = params.contains("mapGuid")
                             ? GUID(params["mapGuid"].get<std::string>())
                             : editContext->TargetMap();
    if (mapGuid.IsNull())
        return Editor::RefuseRequest("No target tilemap — select a tilemap layer entity first");
    TileMap::TileMapData* data = store->GetMap(mapGuid);
    if (!data)
        return Editor::RefuseRequest("Tilemap could not be resolved");

    json out{{"ok", true}};

    if (params.contains("values"))
    {
        const uint32 intGridLayerId = params.value("intGridLayer", 0u);
        TileMap::TileMapLayerData* layer = data->FindLayer(intGridLayerId);
        if (!layer || layer->Type != TileMap::TileMapLayerType::IntGrid)
            return Editor::RefuseRequest("intGridLayer must name an IntGrid layer of this map");
        layer->IntGridValues.clear();
        for (const auto& valueJson : params["values"])
        {
            const uint32 value = valueJson.value("value", 0u);
            if (value == 0u || value > 0xFFu)
                return Editor::RefuseRequest("IntGrid values must be in 1..255");
            layer->IntGridValues.push_back(
                TileMap::IntGridValueDef{static_cast<uint8>(value),
                                         valueJson.value("name", std::string()),
                                         valueJson.value("colorRGBA", 0xFFFFFFFFu)});
        }
        out["values"] = layer->IntGridValues.size();
    }

    if (!params.contains("rulesLayer"))
    {
        store->MarkStructuralChange(mapGuid);
        return out;
    }

    uint32 rulesLayerId = 0;
    if (!TileMapCommands::ReadUInt32(params["rulesLayer"], rulesLayerId))
        return RefuseRequest("rulesLayer must be a non-negative integer layer id");
    TileMap::TileMapLayerData* rulesLayer = data->FindLayer(rulesLayerId);
    if (!rulesLayer || rulesLayer->Type != TileMap::TileMapLayerType::Tiles)
        return Editor::RefuseRequest("rulesLayer must name a Tiles layer of this map");

    uint32 sourceLayerId = rulesLayer->Rules.SourceLayerId;
    if (params.contains("sourceLayer") && !TileMapCommands::ReadUInt32(params["sourceLayer"], sourceLayerId))
        return RefuseRequest("sourceLayer must be a non-negative integer layer id");
    uint32 seed = rulesLayer->Rules.Seed;
    if (params.contains("seed") && !TileMapCommands::ReadUInt32(params["seed"], seed))
        return RefuseRequest("seed must be a non-negative integer");

    if (params.value("clearRules", true))
        rulesLayer->Rules.Groups.clear();
    rulesLayer->Rules.SourceLayerId = sourceLayerId;
    rulesLayer->Rules.Seed = seed;

    const TileMap::TileMapLayerData* source = data->FindLayer(rulesLayer->Rules.SourceLayerId);
    if (!source || source->Type != TileMap::TileMapLayerType::IntGrid)
        return Editor::RefuseRequest("sourceLayer must name an IntGrid layer of this map");

    if (params.contains("groups"))
    {
        for (const auto& groupJson : params["groups"])
        {
            TileMap::TileRuleGroup group;
            group.Id = rulesLayer->Rules.NextGroupId++;
            group.Name = groupJson.value("name", std::string());
            group.Enabled = groupJson.value("enabled", true);
            if (!groupJson.contains("rules") || !groupJson["rules"].is_array())
                return Editor::RefuseRequest("each group needs a rules array");
            for (const auto& ruleJson : groupJson["rules"])
            {
                TileMap::TileRule rule;
                std::string error;
                if (!ParseRuleSpec(ruleJson, rulesLayer->Rules.NextRuleId, rule, error))
                    return Editor::RefuseRequest(error);
                ++rulesLayer->Rules.NextRuleId;
                group.Rules.push_back(std::move(rule));
            }
            rulesLayer->Rules.Groups.push_back(std::move(group));
        }
    }

    if (params.contains("wizard"))
    {
        const auto& wizardJson = params["wizard"];
        const GUID tileSetGuid = wizardJson.contains("tileSetGuid")
                                     ? GUID(wizardJson["tileSetGuid"].get<std::string>())
                                     : rulesLayer->TileSetGuid;
        const TileMap::TileSetAsset* tileSet = store->GetTileSet(tileSetGuid);
        if (!tileSet)
            return Editor::RefuseRequest("wizard needs a resolvable tileset on the rules layer");

        TileMap::RuleWizardRequest request;
        request.TerrainSet = static_cast<int16>(wizardJson.value("terrainSet", 0));
        request.Terrain = static_cast<int16>(wizardJson.value("terrain", 0));
        request.IntGridValue = static_cast<uint8>(wizardJson.value("intGridValue", 1));
        request.Seed = wizardJson.value("seed", rulesLayer->Rules.Seed);
        request.Template = wizardJson.value("template", std::string("corners16")) == "blob47"
                               ? TileMap::RuleTemplate::Blob47
                               : TileMap::RuleTemplate::Corners16;
        TileMap::TileRuleGroup group =
            TileMap::GenerateTerrainRuleGroup(*tileSet, request, rulesLayer->Rules);
        if (group.Rules.empty())
            return Editor::RefuseRequest("wizard produced no rules — check terrainSet/terrain");
        out["wizardRules"] = group.Rules.size();
        rulesLayer->Rules.Groups.push_back(std::move(group));
    }

    std::vector<TileMapECS::TileMapDressedChange> changes;
    if (!store->DressLayer(mapGuid, rulesLayerId, changes))
        return Editor::RefuseRequest("Rule evaluation refused — check sourceLayer");
    store->MarkStructuralChange(mapGuid);
    out["dressedCells"] = changes.size();
    out["groups"] = rulesLayer->Rules.Groups.size();
    return out;
}

json DescribeTileMapRules(const TileMapEditContext* editContext, TileMapECS::TileMapRuntimeStore* store, const json& params)
{
    if (!editContext || !store)
        return Editor::RefuseRequest("No tilemap edit context or runtime store");

    const GUID mapGuid = params.contains("mapGuid")
                             ? GUID(params["mapGuid"].get<std::string>())
                             : editContext->TargetMap();
    const TileMap::TileMapData* data = mapGuid.IsNull() ? nullptr : store->GetMap(mapGuid);
    if (!data)
        return Editor::RefuseRequest("No target tilemap");

    json layers = json::array();
    for (const TileMap::TileMapLayerData& layer : data->Layers())
    {
        json layerJson{{"id", layer.Id},
                       {"name", layer.Name},
                       {"type", layer.Type == TileMap::TileMapLayerType::IntGrid ? "intgrid"
                                                                                 : "tiles"}};
        json values = json::array();
        for (const TileMap::IntGridValueDef& def : layer.IntGridValues)
            values.push_back(json{{"value", def.Value}, {"name", def.Name}});
        layerJson["intGridValues"] = std::move(values);
        layerJson["ruleSourceLayer"] = layer.Rules.SourceLayerId;
        layerJson["ruleSeed"] = layer.Rules.Seed;
        size_t ruleCount = 0;
        for (const TileMap::TileRuleGroup& group : layer.Rules.Groups)
            ruleCount += group.Rules.size();
        layerJson["ruleGroups"] = layer.Rules.Groups.size();
        layerJson["rules"] = ruleCount;
        layers.push_back(std::move(layerJson));
    }

    json out{{"mapGuid", mapGuid.ToString()}, {"layers", std::move(layers)}};

    if (params.contains("window"))
    {
        const auto& windowJson = params["window"];
        const uint32 layerId = params.value("layer", editContext->TargetLayer());
        const TileMap::TileMapLayerData* layer = data->FindLayer(layerId);
        if (!layer)
            return Editor::RefuseRequest("window needs a layer id that exists");
        json window;
        if (!Editor::BuildTileMapRulesWindow(*layer, windowJson, window))
            return Editor::RefuseRequest("window needs ordered int32 integer coordinates and no more than 4096 cells");
        out["window"] = std::move(window);
    }
    return out;
}

} // namespace GameEngine::Editor
