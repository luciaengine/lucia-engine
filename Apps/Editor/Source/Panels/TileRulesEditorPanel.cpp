#include "Panels/TileRulesEditorPanel.h"

#include "TileMap/TileMapEditContext.h"
#include "EditorPanelManager.h"

#include "TileMap/RuleWizard.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileRules.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

// Fresh IntGrid values cycle through this so two values are never the same
// colour by default; the author can still overwrite either.
constexpr uint32 kDefaultValueColors[]{0x4FA3FFFFu, 0xFF8A4CFFu, 0x62D26FFFu,
                                       0xE05C8AFFu, 0xF0C452FFu, 0x9B7BFFFFu};

/**
 * @brief Re-resolves the edited layer on every use.
 *
 * A button handler can fire long after the panel was built, and the map's layer
 * vector moves under add/remove while the reload lane can drop the map outright
 * — so a cached TileMapLayerData* is a dangling pointer waiting to happen.
 */
struct LayerRef
{
    explicit LayerRef(const GUID& mapGuid, uint32 layerId)
    {
        auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
        TileMap::TileMapData* data =
            (store != nullptr && !mapGuid.IsNull()) ? store->GetMap(mapGuid) : nullptr;
        Ptr = data != nullptr ? data->FindLayer(layerId) : nullptr;
    }

    explicit operator bool() const { return Ptr != nullptr; }
    TileMap::TileMapLayerData* operator->() const { return Ptr; }
    TileMap::TileMapLayerData& operator*() const { return *Ptr; }

    TileMap::TileMapLayerData* Ptr = nullptr;
};

LayerRef Layer(const TileRulesEditorPanel::Target& target)
{
    return LayerRef(target.MapGuid, target.LayerId);
}

std::unique_ptr<Label> MakeLabel(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

/** A labelled button. An empty glyph class leaves it text-only, which is what
 *  the rows whose label is authored data (layer names, values, pattern size)
 *  want: a glyph repeated down such a list carries no information. */
std::unique_ptr<Button> MakeSmallButton(const std::string& text, const std::string& tooltip,
                                        const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->AddClass("small");
    button->AddClass("secondary");
    button->SetText(text);
    button->SetTooltip(tooltip);
    if (!glyphClass.empty())
    {
        button->AddClass("tm-icon-label");
        button->AddClass(glyphClass);
    }
    return button;
}

/** A glyph-only button. The tooltip is the only place its name appears. */
std::unique_ptr<Button> MakeIconButton(const std::string& tooltip, const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->AddClass("small");
    button->AddClass("secondary");
    button->AddClass("tm-icon");
    button->AddClass(glyphClass);
    button->SetTooltip(tooltip);
    return button;
}

std::unique_ptr<UIElement> MakeRow()
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("tp-row");
    return row;
}

/** The token a pattern cell shows: what it demands of the value under it. */
std::string PatternCellText(const TileMap::RulePatternCell& cell)
{
    switch (cell.Op)
    {
    case TileMap::RulePatternOp::DontCare:     return ".";
    case TileMap::RulePatternOp::RequireAny:   return "*";
    case TileMap::RulePatternOp::RequireValue: return "=" + std::to_string(cell.Value);
    case TileMap::RulePatternOp::ForbidValue:  return "!" + std::to_string(cell.Value);
    }
    return ".";
}

/** One click advances a pattern cell through the four things it can demand. */
void CyclePatternCell(TileMap::RulePatternCell& cell, uint8 value)
{
    switch (cell.Op)
    {
    case TileMap::RulePatternOp::DontCare:
        cell = TileMap::RulePatternCell{TileMap::RulePatternOp::RequireValue, value};
        return;
    case TileMap::RulePatternOp::RequireValue:
        cell = TileMap::RulePatternCell{TileMap::RulePatternOp::ForbidValue, value};
        return;
    case TileMap::RulePatternOp::ForbidValue:
        cell = TileMap::RulePatternCell{TileMap::RulePatternOp::RequireAny, 0};
        return;
    case TileMap::RulePatternOp::RequireAny:
        cell = TileMap::RulePatternCell{};
        return;
    }
}

/** Resizes a rule's pattern (and stamp) around its centre, keeping what overlaps. */
void ResizeRulePattern(TileMap::TileRule& rule, int32 newSize)
{
    const int32 oldSize = rule.PatternSize;
    const int32 oldRadius = TileMap::RulePatternRadius(oldSize);
    const int32 newRadius = TileMap::RulePatternRadius(newSize);
    std::vector<TileMap::RulePatternCell> pattern(
        static_cast<size_t>(newSize) * static_cast<size_t>(newSize), TileMap::RulePatternCell{});
    std::vector<TileMap::TileCell> stamp(
        static_cast<size_t>(newSize) * static_cast<size_t>(newSize), TileMap::kEmptyCell);

    for (int32 y = 0; y < newSize; ++y)
    {
        for (int32 x = 0; x < newSize; ++x)
        {
            const int32 sourceX = x - newRadius + oldRadius;
            const int32 sourceY = y - newRadius + oldRadius;
            if (sourceX < 0 || sourceY < 0 || sourceX >= oldSize || sourceY >= oldSize)
                continue;
            const size_t from = static_cast<size_t>(sourceY) * static_cast<size_t>(oldSize) +
                                static_cast<size_t>(sourceX);
            const size_t to = static_cast<size_t>(y) * static_cast<size_t>(newSize) +
                              static_cast<size_t>(x);
            if (from < rule.Pattern.size())
                pattern[to] = rule.Pattern[from];
            if (from < rule.Stamp.size())
                stamp[to] = rule.Stamp[from];
        }
    }
    rule.PatternSize = newSize;
    rule.Pattern = std::move(pattern);
    if (rule.OutputMode == TileMap::RuleOutputMode::Stamp)
        rule.Stamp = std::move(stamp);
    else
        rule.Stamp.clear();
}

/** A one-line summary of what a rule paints, for its header. */
std::string RuleOutputSummary(const TileMap::TileRule& rule)
{
    if (rule.OutputMode == TileMap::RuleOutputMode::Stamp)
        return "stamp " + std::to_string(rule.PatternSize) + "x" + std::to_string(rule.PatternSize);
    if (rule.Tiles.empty())
        return "no output";
    std::string text = "tile " + std::to_string(TileMap::CellTileId(rule.Tiles[0].Cell));
    if (rule.Tiles.size() > 1)
        text += " +" + std::to_string(rule.Tiles.size() - 1);
    return text;
}

} // namespace

TileRulesEditorPanel::TileRulesEditorPanel(
    std::shared_ptr<Editor::TileMapEditContext> editContext, EditorPanelManager* panels)
    : DockPanel("Tile Rules"), m_EditContext(std::move(editContext)), m_Panels(panels)
{
    AddClass("tile-palette-panel");
    AddClass("tile-rules-panel");
    RequestSubtreeStyleAssetPath("UI/theme/tile-rules.css");
    RequestSubtreeStyleAssetPath("UI/theme/tilemap-icons.css");
    if (m_Panels)
        m_Panels->RegisterUpdateCallback(
            this, [this](const Editor::PanelUpdateContext&) { Update(); });
}

TileRulesEditorPanel::~TileRulesEditorPanel()
{
    if (m_Panels)
        m_Panels->UnregisterUpdateCallback(this);
}

TileRulesEditorPanel::Target TileRulesEditorPanel::ResolveTarget() const
{
    Target target;
    if (m_EditContext == nullptr)
        return target;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr || m_EditContext->TargetMap().IsNull())
        return target;
    TileMap::TileMapData* data = store->GetMap(m_EditContext->TargetMap());
    if (data == nullptr)
        return target;
    TileMap::TileMapLayerData* layer = data->FindLayer(m_EditContext->TargetLayer());
    if (layer == nullptr)
        return target;

    target.MapGuid = m_EditContext->TargetMap();
    target.LayerId = layer->Id;
    return target;
}

void TileRulesEditorPanel::Update()
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr || m_EditContext == nullptr)
        return;

    const GUID mapGuid = m_EditContext->TargetMap();
    const uint32 layerId = m_EditContext->TargetLayer();
    const uint64 version = store->EditVersion(mapGuid);
    if (!m_RebuildQueued && mapGuid == m_BuiltMap && layerId == m_BuiltLayer &&
        version == m_BuiltVersion)
    {
        return;
    }
    m_RebuildQueued = false;
    m_BuiltMap = mapGuid;
    m_BuiltLayer = layerId;
    m_BuiltVersion = version;
    Rebuild();
}

void TileRulesEditorPanel::SetStatus(const std::string& text)
{
    m_Status = text;
    if (m_StatusLabel != nullptr)
        m_StatusLabel->SetText(text);
}

void TileRulesEditorPanel::ApplyChange(const Target& target)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (store == nullptr || !Layer(target))
        return;

    std::vector<TileMapECS::TileMapDressedChange> changes;
    if (Layer(target)->Type == TileMap::TileMapLayerType::Tiles)
    {
        if (store->DressLayer(target.MapGuid, target.LayerId, changes))
            SetStatus(std::to_string(changes.size()) + " generated tiles updated");
        else
            SetStatus("Rules are unbound — pick a source IntGrid layer");
    }
    // A structural bump is what makes the renderer and every polling panel — this
    // one included — notice an edit that touched no cell at all.
    store->MarkStructuralChange(target.MapGuid);
    m_RebuildQueued = true;
}

void TileRulesEditorPanel::Rebuild()
{
    RemoveAllChildren();
    m_Root = nullptr;
    m_StatusLabel = nullptr;

    auto root = std::make_unique<UIElement>();
    root->AddClass("tp-root");
    root->AddClass("tr-root");

    const Target target = ResolveTarget();
    if (!Layer(target))
    {
        root->AddChild(MakeLabel("tp-empty",
                                 "Select a tilemap layer entity to author its values and rules."));
        m_Root = root.get();
        AddChild(std::move(root));
        return;
    }

    root->AddChild(MakeLabel("tp-row-label",
                             Layer(target)->Name + "  (" +
                                 (Layer(target)->Type == TileMap::TileMapLayerType::IntGrid
                                      ? "IntGrid"
                                      : "Tiles") +
                                 ")"));

    if (Layer(target)->Type == TileMap::TileMapLayerType::IntGrid)
        BuildIntGridValueEditor(*root, *Layer(target));
    else
        BuildRuleEditor(*root, target);

    auto status = MakeLabel("tp-hint", m_Status);
    m_StatusLabel = status.get();
    root->AddChild(std::move(status));

    m_Root = root.get();
    AddChild(std::move(root));
}

void TileRulesEditorPanel::BuildIntGridValueEditor(UIElement& root, TileMap::TileMapLayerData& layer)
{
    const Target target = ResolveTarget();

    auto header = MakeRow();
    header->AddChild(MakeLabel("tp-row-label", "Values"));
    auto add = MakeSmallButton("Add Value", "Define another value on this layer",
                               "tm-glyph-add");
    add->RegisterEventHandler(kEventButtonClick, [this, target](UIEvent&)
                    {
                        if (!Layer(target))
                            return;
                        uint8 next = 1;
                        for (const TileMap::IntGridValueDef& def : Layer(target)->IntGridValues)
                            next = std::max<uint8>(next, static_cast<uint8>(def.Value + 1));
                        const size_t index = Layer(target)->IntGridValues.size();
                        Layer(target)->IntGridValues.push_back(TileMap::IntGridValueDef{
                            next, "Value " + std::to_string(next),
                            kDefaultValueColors[index % std::size(kDefaultValueColors)]});
                        ApplyChange(target);
                    });
    header->AddChild(std::move(add));
    root.AddChild(std::move(header));

    for (size_t index = 0; index < layer.IntGridValues.size(); ++index)
    {
        const TileMap::IntGridValueDef& def = layer.IntGridValues[index];
        auto row = MakeRow();
        row->AddChild(MakeLabel("tp-row-label", std::to_string(def.Value)));

        auto name = std::make_unique<TextField>();
        name->AddClass("tr-name-field");
        name->SetValueWithoutNotify(def.Name);
        name->SetTooltip("What this value means");
        name->SetOnValueChanged([target, index](const std::string& text)
                                {
                                    if (!Layer(target) ||
                                        index >= Layer(target)->IntGridValues.size())
                                        return;
                                    Layer(target)->IntGridValues[index].Name = text;
                                });
        row->AddChild(std::move(name));

        auto colour = MakeIconButton("Cycle this value's tint color", "tm-glyph-colour");
        colour->RegisterEventHandler(kEventButtonClick, [this, target, index](UIEvent&)
                           {
                               if (!Layer(target) || index >= Layer(target)->IntGridValues.size())
                                   return;
                               TileMap::IntGridValueDef& def_ = Layer(target)->IntGridValues[index];
                               size_t slot = 0;
                               while (slot < std::size(kDefaultValueColors) &&
                                      kDefaultValueColors[slot] != def_.ColorRGBA)
                               {
                                   ++slot;
                               }
                               def_.ColorRGBA =
                                   kDefaultValueColors[(slot + 1) % std::size(kDefaultValueColors)];
                               ApplyChange(target);
                           });
        row->AddChild(std::move(colour));

        auto remove =
            MakeIconButton("Drop this value (painted cells keep the number)", "tm-glyph-remove");
        remove->RegisterEventHandler(kEventButtonClick, [this, target, index](UIEvent&)
                           {
                               if (!Layer(target) || index >= Layer(target)->IntGridValues.size())
                                   return;
                               Layer(target)->IntGridValues.erase(
                                   Layer(target)->IntGridValues.begin() +
                                   static_cast<long>(index));
                               ApplyChange(target);
                           });
        row->AddChild(std::move(remove));
        root.AddChild(std::move(row));
    }

    if (layer.IntGridValues.empty())
        root.AddChild(MakeLabel("tp-empty", "No values yet — add one to start painting meaning."));
}

void TileRulesEditorPanel::BuildSourceRow(UIElement& root, const Target& target)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    TileMap::TileMapData* data = store ? store->GetMap(target.MapGuid) : nullptr;
    if (data == nullptr)
        return;

    auto row = MakeRow();
    row->AddChild(MakeLabel("tp-row-label", "Source"));

    bool anyIntGrid = false;
    for (const TileMap::TileMapLayerData& candidate : data->Layers())
    {
        if (candidate.Type != TileMap::TileMapLayerType::IntGrid)
            continue;
        anyIntGrid = true;
        const uint32 candidateId = candidate.Id;
        auto button =
            MakeSmallButton(candidate.Name, "Read this IntGrid layer's values", std::string());
        if (Layer(target)->Rules.SourceLayerId == candidateId)
            button->AddClass("selected");
        button->RegisterEventHandler(kEventButtonClick, [this, target, candidateId](UIEvent&)
                           {
                               if (!Layer(target))
                                   return;
                               Layer(target)->Rules.SourceLayerId = candidateId;
                               ApplyChange(target);
                           });
        row->AddChild(std::move(button));
    }
    if (!anyIntGrid)
        row->AddChild(MakeLabel("tp-empty", "This map has no IntGrid layer yet."));
    root.AddChild(std::move(row));

    auto seedRow = MakeRow();
    seedRow->AddChild(MakeLabel("tp-row-label", "Seed"));
    auto seed = std::make_unique<IntField>();
    seed->AddClass("tr-number-field");
    seed->SetRange(0, 1 << 20);
    seed->SetValueWithoutNotify(static_cast<int>(Layer(target)->Rules.Seed));
    seed->SetTooltip("Re-rolls every weighted pick and chance test, deterministically");
    seed->SetOnValueChanged([this, target](const int& value)
                            {
                                if (!Layer(target))
                                    return;
                                Layer(target)->Rules.Seed = static_cast<uint32>(std::max(0, value));
                                ApplyChange(target);
                            });
    seedRow->AddChild(std::move(seed));
    root.AddChild(std::move(seedRow));
}

void TileRulesEditorPanel::BuildWizardRow(UIElement& root, const Target& target)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const TileMap::TileSetAsset* tileSet =
        store ? store->GetTileSet(Layer(target)->TileSetGuid) : nullptr;
    if (tileSet == nullptr || tileSet->TerrainSets().empty())
        return;

    auto row = MakeRow();
    row->AddChild(MakeLabel("tp-row-label", "Wizard"));
    for (size_t setIndex = 0; setIndex < tileSet->TerrainSets().size(); ++setIndex)
    {
        const TileMap::TerrainSetDef& set = tileSet->TerrainSets()[setIndex];
        for (size_t terrainIndex = 0; terrainIndex < set.Terrains.size(); ++terrainIndex)
        {
            const std::string label =
                (set.Terrains[terrainIndex].Name.empty()
                     ? ("Terrain " + std::to_string(terrainIndex))
                     : set.Terrains[terrainIndex].Name);
            const bool blob = set.MatchMode != TileMap::TerrainMatchMode::Corners;
            auto button = MakeSmallButton(
                "Generate " + label,
                "Build a rule group from this terrain's edge and corner matches, keyed on IntGrid value " +
                    std::to_string(m_PatternValue),
                "tm-glyph-wand");
            const int16 terrainSet = static_cast<int16>(setIndex);
            const int16 terrain = static_cast<int16>(terrainIndex);
            button->RegisterEventHandler(kEventButtonClick, [this, target, terrainSet, terrain, blob](UIEvent&)
                               {
                                   auto* store_ = TileMapECS::TileMapRuntimeStore::TryGet();
                                   const TileMap::TileSetAsset* tileSet_ =
                                       store_ ? store_->GetTileSet(Layer(target)->TileSetGuid)
                                              : nullptr;
                                   if (!Layer(target) || tileSet_ == nullptr)
                                       return;
                                   TileMap::RuleWizardRequest request;
                                   request.TerrainSet = terrainSet;
                                   request.Terrain = terrain;
                                   request.IntGridValue = m_PatternValue;
                                   request.Seed = Layer(target)->Rules.Seed;
                                   request.Template = blob ? TileMap::RuleTemplate::Blob47
                                                           : TileMap::RuleTemplate::Corners16;
                                   TileMap::TileRuleGroup group = TileMap::GenerateTerrainRuleGroup(
                                       *tileSet_, request, Layer(target)->Rules);
                                   if (group.Rules.empty())
                                   {
                                       SetStatus("The wizard found no tiles for that terrain");
                                       return;
                                   }
                                   Layer(target)->Rules.Groups.push_back(std::move(group));
                                   ApplyChange(target);
                               });
            row->AddChild(std::move(button));
        }
    }
    root.AddChild(std::move(row));
}

void TileRulesEditorPanel::BuildPatternValueRow(UIElement& root,
                                                const TileMap::TileMapLayerData& source)
{
    auto row = MakeRow();
    row->AddChild(MakeLabel("tp-row-label", "Pattern value"));
    for (const TileMap::IntGridValueDef& def : source.IntGridValues)
    {
        const uint8 value = def.Value;
        auto button = MakeSmallButton(
            (def.Name.empty() ? ("Value " + std::to_string(value)) : def.Name),
            "Pattern cells clicked from now on test for this value", std::string());
        if (m_PatternValue == value)
            button->AddClass("selected");
        button->RegisterEventHandler(kEventButtonClick, [this, value](UIEvent&)
                           {
                               m_PatternValue = value;
                               m_RebuildQueued = true;
                           });
        row->AddChild(std::move(button));
    }
    if (source.IntGridValues.empty())
        row->AddChild(MakeLabel("tp-empty", "The source layer defines no values yet."));
    root.AddChild(std::move(row));
}

void TileRulesEditorPanel::BuildRuleEditor(UIElement& root, const Target& target)
{
    BuildSourceRow(root, target);

    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    TileMap::TileMapData* data = store ? store->GetMap(target.MapGuid) : nullptr;
    const TileMap::TileMapLayerData* source =
        data ? data->FindLayer(Layer(target)->Rules.SourceLayerId) : nullptr;
    if (source != nullptr && source->Type == TileMap::TileMapLayerType::IntGrid)
    {
        // The palette's chip choice is the natural default, so the two surfaces
        // agree unless this panel is deliberately pointed elsewhere.
        if (m_EditContext != nullptr && m_EditContext->IntGridValue() != 0 &&
            source->FindIntGridValue(m_EditContext->IntGridValue()) != nullptr &&
            source->FindIntGridValue(m_PatternValue) == nullptr)
        {
            m_PatternValue = m_EditContext->IntGridValue();
        }
        BuildPatternValueRow(root, *source);
    }

    BuildWizardRow(root, target);

    auto header = MakeRow();
    header->AddChild(MakeLabel("tp-row-label", "Groups"));
    auto addGroup = MakeSmallButton("Add Group", "Append an empty rule group", "tm-glyph-add");
    addGroup->RegisterEventHandler(kEventButtonClick, [this, target](UIEvent&)
                         {
                             if (!Layer(target))
                                 return;
                             TileMap::TileRuleGroup group;
                             group.Id = Layer(target)->Rules.NextGroupId++;
                             group.Name = "Group " +
                                          std::to_string(Layer(target)->Rules.Groups.size() + 1);
                             Layer(target)->Rules.Groups.push_back(std::move(group));
                             ApplyChange(target);
                         });
    header->AddChild(std::move(addGroup));

    auto save = MakeSmallButton("Save Map", "Write the tilemap asset back to disk",
                                "tm-glyph-save");
    save->RegisterEventHandler(kEventButtonClick, [this, target](UIEvent&)
                     {
                         auto* store_ = TileMapECS::TileMapRuntimeStore::TryGet();
                         if (store_ == nullptr || !Layer(target))
                             return;
                         SetStatus(store_->SaveMap(target.MapGuid) ? "Saved" : "Save failed");
                     });
    header->AddChild(std::move(save));
    root.AddChild(std::move(header));

    for (size_t groupIndex = 0; groupIndex < Layer(target)->Rules.Groups.size(); ++groupIndex)
        root.AddChild(BuildGroupBlock(target, groupIndex));

    if (Layer(target)->Rules.Groups.empty())
        root.AddChild(MakeLabel("tp-empty",
                                "No rules yet — generate a group from a terrain set, or add one."));
}

std::unique_ptr<UIElement> TileRulesEditorPanel::BuildGroupBlock(const Target& target,
                                                                 size_t groupIndex)
{
    TileMap::TileRuleGroup& group = Layer(target)->Rules.Groups[groupIndex];

    auto block = std::make_unique<UIElement>();
    block->AddClass("tr-group");

    auto header = MakeRow();
    auto name = std::make_unique<TextField>();
    name->AddClass("tr-name-field");
    name->SetValueWithoutNotify(group.Name);
    name->SetOnValueChanged([target, groupIndex](const std::string& text)
                            {
                                if (Layer(target) &&
                                    groupIndex < Layer(target)->Rules.Groups.size())
                                {
                                    Layer(target)->Rules.Groups[groupIndex].Name = text;
                                }
                            });
    header->AddChild(std::move(name));

    auto enabled = std::make_unique<Checkbox>();
    enabled->SetText("On");
    enabled->SetTooltip("Disabled groups are skipped entirely");
    enabled->SetValueWithoutNotify(group.Enabled);
    enabled->SetOnValueChanged([this, target, groupIndex](const bool& on)
                               {
                                   if (!Layer(target) ||
                                       groupIndex >= Layer(target)->Rules.Groups.size())
                                       return;
                                   Layer(target)->Rules.Groups[groupIndex].Enabled = on;
                                   ApplyChange(target);
                               });
    header->AddChild(std::move(enabled));

    const auto moveGroup = [this, target, groupIndex](int32 delta)
    {
        auto& groups = Layer(target)->Rules.Groups;
        const size_t other = static_cast<size_t>(static_cast<int64>(groupIndex) + delta);
        if (!Layer(target) || groupIndex >= groups.size() || other >= groups.size())
            return;
        std::swap(groups[groupIndex], groups[other]);
        ApplyChange(target);
    };
    auto up = MakeIconButton("Evaluate this group earlier", "tm-glyph-move-up");
    up->RegisterEventHandler(kEventButtonClick, [moveGroup](UIEvent&) { moveGroup(-1); });
    header->AddChild(std::move(up));
    auto down = MakeIconButton("Evaluate this group later", "tm-glyph-move-down");
    down->RegisterEventHandler(kEventButtonClick, [moveGroup](UIEvent&) { moveGroup(1); });
    header->AddChild(std::move(down));

    auto addRule = MakeSmallButton("Add Rule", "Append a 3x3 rule to this group", "tm-glyph-add");
    addRule->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex](UIEvent&)
                        {
                            if (!Layer(target) || groupIndex >= Layer(target)->Rules.Groups.size())
                                return;
                            TileMap::TileRule rule;
                            rule.Id = Layer(target)->Rules.NextRuleId++;
                            rule.Name = "Rule " + std::to_string(rule.Id);
                            rule.PatternSize = 3;
                            rule.Pattern.assign(9, TileMap::RulePatternCell{});
                            rule.Pattern[4] = TileMap::RulePatternCell{
                                TileMap::RulePatternOp::RequireValue, m_PatternValue};
                            rule.Tiles.push_back(TileMap::RuleTileOutput{TileMap::kEmptyCell, 1.0f});
                            Layer(target)->Rules.Groups[groupIndex].Rules.push_back(std::move(rule));
                            ApplyChange(target);
                        });
    header->AddChild(std::move(addRule));

    auto removeGroup = MakeIconButton("Delete this group and its rules", "tm-glyph-remove");
    removeGroup->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex](UIEvent&)
                            {
                                auto& groups = Layer(target)->Rules.Groups;
                                if (!Layer(target) || groupIndex >= groups.size())
                                    return;
                                groups.erase(groups.begin() + static_cast<long>(groupIndex));
                                ApplyChange(target);
                            });
    header->AddChild(std::move(removeGroup));
    block->AddChild(std::move(header));

    for (size_t ruleIndex = 0; ruleIndex < group.Rules.size(); ++ruleIndex)
        block->AddChild(BuildRuleBlock(target, groupIndex, ruleIndex));

    return block;
}

std::unique_ptr<UIElement> TileRulesEditorPanel::BuildPatternGrid(const Target& target,
                                                                  size_t groupIndex,
                                                                  size_t ruleIndex)
{
    const TileMap::TileRule& rule = Layer(target)->Rules.Groups[groupIndex].Rules[ruleIndex];

    auto grid = std::make_unique<UIElement>();
    grid->AddClass("tr-pattern");

    for (int32 y = 0; y < rule.PatternSize; ++y)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("tr-pattern-row");
        for (int32 x = 0; x < rule.PatternSize; ++x)
        {
            const size_t index = static_cast<size_t>(y) *
                                     static_cast<size_t>(rule.PatternSize) +
                                 static_cast<size_t>(x);
            auto cell = std::make_unique<Button>();
            cell->AddClass("small");
            cell->AddClass("secondary");
            cell->AddClass("tr-pattern-cell");
            cell->SetText(PatternCellText(rule.Pattern[index]));
            cell->SetTooltip("Click to cycle: don't care, require, forbid, any");
            cell->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex, ruleIndex, index](UIEvent&)
                             {
                                 if (!Layer(target) ||
                                     groupIndex >= Layer(target)->Rules.Groups.size())
                                     return;
                                 auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                                 if (ruleIndex >= rules.size() ||
                                     index >= rules[ruleIndex].Pattern.size())
                                     return;
                                 CyclePatternCell(rules[ruleIndex].Pattern[index], m_PatternValue);
                                 ApplyChange(target);
                             });
            row->AddChild(std::move(cell));
        }
        grid->AddChild(std::move(row));
    }
    return grid;
}

std::unique_ptr<UIElement> TileRulesEditorPanel::BuildRuleBlock(const Target& target,
                                                                size_t groupIndex,
                                                                size_t ruleIndex)
{
    const TileMap::TileRule& rule = Layer(target)->Rules.Groups[groupIndex].Rules[ruleIndex];

    auto block = std::make_unique<UIElement>();
    block->AddClass("tr-rule");

    auto header = MakeRow();
    header->AddChild(MakeLabel("tp-row-label", rule.Name));
    header->AddChild(MakeLabel("tr-rule-output", RuleOutputSummary(rule)));

    auto size = MakeSmallButton(std::to_string(rule.PatternSize) + "x" +
                                    std::to_string(rule.PatternSize),
                                "Cycle the pattern size: 1, 3, 5", std::string());
    size->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex, ruleIndex](UIEvent&)
                     {
                         if (!Layer(target) || groupIndex >= Layer(target)->Rules.Groups.size())
                             return;
                         auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                         if (ruleIndex >= rules.size())
                             return;
                         const int32 next = rules[ruleIndex].PatternSize >= 5
                                                ? 1
                                                : rules[ruleIndex].PatternSize + 2;
                         ResizeRulePattern(rules[ruleIndex], next);
                         ApplyChange(target);
                     });
    header->AddChild(std::move(size));

    auto fromStamp = MakeSmallButton("From Palette",
                                     "Take the palette selection as this rule's output",
                                     "tm-glyph-from-palette");
    fromStamp->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex, ruleIndex](UIEvent&)
                          {
                              if (!Layer(target) || m_EditContext == nullptr ||
                                  groupIndex >= Layer(target)->Rules.Groups.size())
                                  return;
                              auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                              if (ruleIndex >= rules.size())
                                  return;
                              TileMap::TileRule& rule_ = rules[ruleIndex];
                              const Editor::TileStamp& stamp = m_EditContext->Stamp();
                              if (stamp.IsEmpty())
                              {
                                  SetStatus("Pick tiles in the palette first");
                                  return;
                              }
                              // A palette selection the size of the pattern is a
                              // stamp; anything else is a single-tile output.
                              if (stamp.Width == rule_.PatternSize &&
                                  stamp.Height == rule_.PatternSize)
                              {
                                  rule_.OutputMode = TileMap::RuleOutputMode::Stamp;
                                  rule_.Stamp = stamp.Cells;
                                  rule_.Tiles.clear();
                              }
                              else
                              {
                                  rule_.OutputMode = TileMap::RuleOutputMode::WeightedTiles;
                                  rule_.Stamp.clear();
                                  rule_.Tiles.clear();
                                  for (const TileMap::TileCell cell : stamp.Cells)
                                  {
                                      if (!TileMap::CellIsEmpty(cell))
                                          rule_.Tiles.push_back(
                                              TileMap::RuleTileOutput{cell, 1.0f});
                                  }
                                  if (rule_.Tiles.empty())
                                      rule_.Tiles.push_back(
                                          TileMap::RuleTileOutput{TileMap::kEmptyCell, 1.0f});
                              }
                              ApplyChange(target);
                          });
    header->AddChild(std::move(fromStamp));

    const auto moveRule = [this, target, groupIndex, ruleIndex](int32 delta)
    {
        if (!Layer(target) || groupIndex >= Layer(target)->Rules.Groups.size())
            return;
        auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
        const size_t other = static_cast<size_t>(static_cast<int64>(ruleIndex) + delta);
        if (ruleIndex >= rules.size() || other >= rules.size())
            return;
        std::swap(rules[ruleIndex], rules[other]);
        ApplyChange(target);
    };
    auto up = MakeIconButton("Evaluate this rule earlier", "tm-glyph-move-up");
    up->RegisterEventHandler(kEventButtonClick, [moveRule](UIEvent&) { moveRule(-1); });
    header->AddChild(std::move(up));
    auto down = MakeIconButton("Evaluate this rule later", "tm-glyph-move-down");
    down->RegisterEventHandler(kEventButtonClick, [moveRule](UIEvent&) { moveRule(1); });
    header->AddChild(std::move(down));

    auto remove = MakeIconButton("Delete this rule", "tm-glyph-remove");
    remove->RegisterEventHandler(kEventButtonClick, [this, target, groupIndex, ruleIndex](UIEvent&)
                       {
                           if (!Layer(target) || groupIndex >= Layer(target)->Rules.Groups.size())
                               return;
                           auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                           if (ruleIndex >= rules.size())
                               return;
                           rules.erase(rules.begin() + static_cast<long>(ruleIndex));
                           ApplyChange(target);
                       });
    header->AddChild(std::move(remove));
    block->AddChild(std::move(header));

    block->AddChild(BuildPatternGrid(target, groupIndex, ruleIndex));

    auto options = MakeRow();

    auto breakOnMatch = std::make_unique<Checkbox>();
    breakOnMatch->SetText("Break");
    breakOnMatch->SetTooltip("Stop evaluating this cell once the rule matches");
    breakOnMatch->SetValueWithoutNotify(rule.BreakOnMatch);
    breakOnMatch->SetOnValueChanged([this, target, groupIndex, ruleIndex](const bool& on)
                                    {
                                        if (!Layer(target) ||
                                            groupIndex >= Layer(target)->Rules.Groups.size())
                                            return;
                                        auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                                        if (ruleIndex >= rules.size())
                                            return;
                                        rules[ruleIndex].BreakOnMatch = on;
                                        ApplyChange(target);
                                    });
    options->AddChild(std::move(breakOnMatch));

    const auto addMirror = [&](const char* text, const char* tooltip, bool current, bool onX)
    {
        auto mirror = std::make_unique<Checkbox>();
        mirror->SetText(text);
        mirror->SetTooltip(tooltip);
        mirror->SetValueWithoutNotify(current);
        mirror->SetOnValueChanged([this, target, groupIndex, ruleIndex, onX](const bool& on)
                                  {
                                      if (!Layer(target) ||
                                          groupIndex >= Layer(target)->Rules.Groups.size())
                                          return;
                                      auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                                      if (ruleIndex >= rules.size())
                                          return;
                                      if (onX)
                                          rules[ruleIndex].MirrorX = on;
                                      else
                                          rules[ruleIndex].MirrorY = on;
                                      ApplyChange(target);
                                  });
        options->AddChild(std::move(mirror));
    };
    addMirror("Mirror X", "Also try the pattern mirrored on X, flipping the output", rule.MirrorX,
              true);
    addMirror("Mirror Y", "Also try the pattern mirrored on Y, flipping the output", rule.MirrorY,
              false);

    auto chance = std::make_unique<IntField>();
    chance->AddClass("tr-number-field");
    chance->SetRange(0, 100);
    chance->SetSuffix("%");
    chance->SetValueWithoutNotify(static_cast<int>(rule.Chance * 100.0f + 0.5f));
    chance->SetTooltip("How often the rule fires where it matches");
    chance->SetOnValueChanged([this, target, groupIndex, ruleIndex](const int& percent)
                              {
                                  if (!Layer(target) ||
                                      groupIndex >= Layer(target)->Rules.Groups.size())
                                      return;
                                  auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                                  if (ruleIndex >= rules.size())
                                      return;
                                  rules[ruleIndex].Chance =
                                      static_cast<float32>(std::clamp(percent, 0, 100)) / 100.0f;
                                  ApplyChange(target);
                              });
    options->AddChild(std::move(chance));

    const auto addModulo = [&](const char* tooltip, int32 current, bool onX)
    {
        auto field = std::make_unique<IntField>();
        field->AddClass("tr-number-field");
        field->SetRange(1, 64);
        field->SetValueWithoutNotify(current);
        field->SetTooltip(tooltip);
        field->SetOnValueChanged([this, target, groupIndex, ruleIndex, onX](const int& value)
                                 {
                                     if (!Layer(target) ||
                                         groupIndex >= Layer(target)->Rules.Groups.size())
                                         return;
                                     auto& rules = Layer(target)->Rules.Groups[groupIndex].Rules;
                                     if (ruleIndex >= rules.size())
                                         return;
                                     if (onX)
                                         rules[ruleIndex].ModuloX = std::max(1, value);
                                     else
                                         rules[ruleIndex].ModuloY = std::max(1, value);
                                     ApplyChange(target);
                                 });
        options->AddChild(std::move(field));
    };
    options->AddChild(MakeLabel("tp-row-label", "Every"));
    addModulo("Fire only on every Nth column", rule.ModuloX, true);
    addModulo("Fire only on every Nth row", rule.ModuloY, false);

    block->AddChild(std::move(options));
    return block;
}

} // namespace GameEngine
