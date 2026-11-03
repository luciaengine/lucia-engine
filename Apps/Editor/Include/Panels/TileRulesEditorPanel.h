#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "UI/Controls/DockPanel.h"

#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{

namespace TileMap
{
class TileMapLayerData;
struct TileRule;
struct TileRuleGroup;
} // namespace TileMap

namespace Editor
{
class TileMapEditContext;
} // namespace Editor

class EditorPanelManager;
class Label;
class UIElement;

/**
 * @brief Authoring surface for IntGrid semantics and the rules that dress a
 * Tiles layer from them.
 *
 * The panel edits the resident map through TileMapRuntimeStore — the same data
 * the renderer reads — and re-dresses the target layer after every change, so
 * the viewport shows the consequence of a rule edit immediately. It owns no map
 * of its own and caches nothing beyond the signature it rebuilds on.
 */
class TileRulesEditorPanel : public DockPanel
{
public:
    TileRulesEditorPanel(std::shared_ptr<Editor::TileMapEditContext> editContext,
                         EditorPanelManager* panels);
    ~TileRulesEditorPanel() override;

    std::string_view DeclaredTabIconClass() const override { return "dock-tile-rules-icon"; }

    /** Per-frame poll, registered with EditorPanelManager. */
    void Update();

    /**
     * @brief Which map and layer the panel edits — ids only, never a pointer:
     * handlers outlive the vector the layer lives in.
     */
    struct Target
    {
        GUID MapGuid;
        uint32 LayerId = 0;
    };

private:
    Target ResolveTarget() const;
    void Rebuild();
    void BuildIntGridValueEditor(UIElement& root, TileMap::TileMapLayerData& layer);
    void BuildRuleEditor(UIElement& root, const Target& target);
    void BuildSourceRow(UIElement& root, const Target& target);
    void BuildWizardRow(UIElement& root, const Target& target);
    void BuildPatternValueRow(UIElement& root, const TileMap::TileMapLayerData& source);
    std::unique_ptr<UIElement> BuildGroupBlock(const Target& target, size_t groupIndex);
    std::unique_ptr<UIElement> BuildRuleBlock(const Target& target, size_t groupIndex,
                                              size_t ruleIndex);
    std::unique_ptr<UIElement> BuildPatternGrid(const Target& target, size_t groupIndex,
                                                size_t ruleIndex);

    /** Re-dresses the target layer and queues a rebuild; every mutation ends here. */
    void ApplyChange(const Target& target);
    void SetStatus(const std::string& text);

    std::shared_ptr<Editor::TileMapEditContext> m_EditContext;
    EditorPanelManager* m_Panels = nullptr;
    UIElement* m_Root = nullptr;
    Label* m_StatusLabel = nullptr;

    // Which IntGrid value the pattern-cell cycle writes; mirrors the palette's
    // chip selection unless the panel's own value row overrides it.
    uint8 m_PatternValue = 1;

    // Signature of what is on screen: a rebuild is a full teardown, so it only
    // happens when the map, the layer, or the store's edit version moved.
    GUID m_BuiltMap;
    uint32 m_BuiltLayer = 0;
    uint64 m_BuiltVersion = 0;
    bool m_RebuildQueued = false;

    std::string m_Status;
};

} // namespace GameEngine
