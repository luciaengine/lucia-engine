#pragma once

#include "AssetCore/GUID.h"
#include "TileMap/TileSetDocument.h"
#include "UI/Controls/DockPanel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{

namespace TileMap
{
class TileSetAsset;
struct TileSetTile;
}

class AssetField;
class ConfirmActionModal;
class Dropdown;
class IntField;
class Button;
class Label;
class TileSetCollisionEditor;
class TileSetTerrainBitEditor;
class UIElement;
struct EditorContext;

/**
 * @brief Canvas that draws a tile set's atlas texture with its slice grid on top.
 *
 * Lives in the header only because TileSetEditorPanel holds a typed pointer to
 * it. It resolves the atlas dimensions itself from the UI texture cache, so the
 * panel never has to push a size it may not know yet.
 */
class TileSetAtlasPreview : public UIElement
{
public:
    /** Atlas texture to draw. A null GUID leaves the canvas empty. */
    void SetAtlas(const GUID& textureGuid);

    /** Slice parameters, in atlas pixels, used to lay the grid overlay out. */
    void SetGrid(int32_t tileWidthPx, int32_t tileHeightPx, int32_t marginPx, int32_t spacingPx);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    GUID m_TextureGuid;
    int32_t m_TileWidthPx = 0;
    int32_t m_TileHeightPx = 0;
    int32_t m_MarginPx = 0;
    int32_t m_SpacingPx = 0;
};

/**
 * @brief Dock panel for authoring `.tileset` assets: pick an atlas texture,
 * set the grid (tile size, margin, spacing), see the slice live, then save.
 *
 * The panel owns the edited TileSetAsset outright rather than mutating a shared
 * loaded instance, so an unapplied edit can never leak into the running scene.
 */
class TileSetEditorPanel : public DockPanel
{
public:
    TileSetEditorPanel();
    ~TileSetEditorPanel() override;

    std::string_view DeclaredTabIconClass() const override { return "dock-tile-set-editor-icon"; }

    /**
     * Wires the editor services the panel needs (asset registry, thumbnails,
     * assets root). Borrowed: the context must outlive the panel.
     */
    void SetContext(const EditorContext* ctx);

    /** Loads an existing `.tileset` by GUID and shows it for editing. */
    void OpenTileSet(const GUID& tileSetGuid);

    /** Loads an existing `.tileset` by path and shows it for editing. */
    void OpenTileSetPath(const std::filesystem::path& path);

    /**
     * Opens the tile set belonging to a texture (the texture's own path with a
     * `.tileset` extension), reading the file when one is already there and
     * writing a fresh one only when there is none.
     */
    void OpenOrCreateForTexture(const GUID& textureGuid);

    /**
     * Re-slices the open tile set with the form's values and writes the file.
     * False on failure, with the reason in the panel's status line. A re-slice
     * that would strip authored data off tiles asks first and returns false
     * until the author confirms it.
     */
    bool ApplyAndSave();

    /**
     * The tile set being authored, for automation that fills in terrain data
     * the canvas would otherwise be clicked to produce. Null when none is open.
     */
    TileMap::TileSetAsset* EditedTileSet() { return m_Document.Asset(); }

    /**
     * Colour a newly added terrain takes, as 0xRRGGBBAA. Terrain colours only
     * have to be told apart in the authoring overlay, so they come from a fixed
     * ring rather than from a picker the user must visit before annotating a
     * single tile.
     */
    static uint32_t DefaultTerrainColor(size_t terrainIndex);

    /**
     * Per-frame poll, registered with EditorPanelManager. Applies row rebuilds
     * queued from inside a row's own handler, which cannot tear its row down
     * while the event that fired it is still dispatching.
     */
    void Update();

protected:
    void OnOwnerManagerChanged(UIManager* owner) override;

private:
    void BuildLayout();
    std::unique_ptr<UIElement> BuildTerrainSection();
    std::unique_ptr<UIElement> BuildCollisionSection();

    /** The open tile set, or null when none is bound. */
    TileMap::TileSetAsset* TileSet() { return m_Document.Asset(); }

    /** The tile the collision canvas has selected, or null when none/no tile set. */
    TileMap::TileSetTile* SelectedCollisionTile();

    /** Mirrors the selected tile's collision mode and material into the tools row. */
    void ShowSelectedTileCollision();

    void OnSelectedTileCollisionModeChanged(const std::string& value);
    void OnSelectedTilePhysicsMaterialChanged(const GUID& materialGuid);
    /**
     * Rebuilds the frame rows for the selected tile. A tile with frames plays
     * them in a loop at runtime (TileSetGpuTables carries the walk); each frame
     * names another tile of this set and how long it shows.
     */
    void ShowSelectedTileAnimation();
    void AddAnimationFrameToSelectedTile();
    void RemoveAnimationFrameFromSelectedTile(size_t frameIndex);

    /** Rebuilds the terrain-set and terrain rows from the loaded asset. */
    void RebuildTerrainRows();
    void InvalidateTerrainRows() { m_TerrainRowsDirty = true; }

    /** Arms the terrain the bit-editor canvas paints, and highlights its row. */
    void SetActiveTerrain(int16_t terrainSet, int16_t terrain);

    /** Pushes the loaded asset into the terrain canvas and rebuilds its rows. */
    void ShowTerrainState();

    /** Pushes the loaded asset's grid values into the controls without re-firing their callbacks. */
    void ShowLoadedAsset();

    void OnAtlasTextureChanged(const GUID& textureGuid);
    void OnGridFieldChanged();

    /** Asks the UI texture cache for the atlas size; the answer arrives on a later frame. */
    void RequestAtlasSize();
    void OnAtlasSizeResolved(int32_t widthPx, int32_t heightPx);

    /** Decodes the atlas image to read its dimensions, independently of any GPU upload. */
    bool ResolveAtlasSizeFromFile(const GUID& textureGuid, int32_t& outWidthPx,
                                  int32_t& outHeightPx) const;

    void UpdateSummary();
    void SetStatus(const std::string& text, bool isError);

    /** The four grid values, read out of the form's fields. */
    struct GridSpec
    {
        int32_t TileWidthPx = 0;
        int32_t TileHeightPx = 0;
        int32_t MarginPx = 0;
        int32_t SpacingPx = 0;
    };

    GridSpec FormGrid() const;

    /** Re-slices the open tile set to `grid` and writes the file. */
    bool WriteWithGrid(const GridSpec& grid);

    /**
     * Creates a confirmation and parents it to the UI root, falling back to this
     * panel when there is no root. Root-parented is what lets a confirmation
     * survive the panel being closed or re-docked while it is up, and is why
     * every callback on one must be guarded by m_AsyncLifetime.
     */
    ConfirmActionModal* AttachConfirmModal();

    /**
     * Builds the destructive-re-slice confirmation on first use. Returns false
     * when it could not be built — the caller must then NOT write: a
     * confirmation that cannot be shown is a reason to stop, never a reason to
     * proceed unconfirmed.
     */
    bool EnsureReSliceConfirmModal();

    /**
     * Builds the changed-on-disk confirmation on first use. Same contract as
     * EnsureReSliceConfirmModal: false means the caller must not write.
     */
    bool EnsureDiskConflictModal();

    /**
     * Offers to overwrite a file that changed under this document. Returns false
     * always — nothing is written on this pass; confirming writes `grid` later.
     */
    bool OfferDiskConflictOverwrite(const GridSpec& grid);

    std::filesystem::path AssetPathForGuid(const GUID& guid) const;

    /**
     * The `.tileset` this atlas texture belongs to: the one already bound to it
     * if the project has one, otherwise the stem-derived path a new one would
     * take. Every site that turns an atlas into a path goes through here — the
     * stem alone silently orphans a renamed tile set and creates a blank one
     * beside it.
     */
    std::filesystem::path TileSetPathForAtlas(const GUID& textureGuid) const;

    int32_t GridValue(const IntField* field, int32_t fallback) const;

    const EditorContext* m_Context = nullptr;

    Editor::TileSetDocument m_Document;

    // Atlas pixel size. The UI texture cache fills it in asynchronously for the
    // preview; a save that needs it before then decodes the image file instead.
    int32_t m_AtlasWidthPx = 0;
    int32_t m_AtlasHeightPx = 0;

    AssetField* m_AtlasField = nullptr;
    IntField* m_TileWidthField = nullptr;
    IntField* m_TileHeightField = nullptr;
    IntField* m_MarginField = nullptr;
    IntField* m_SpacingField = nullptr;
    Label* m_SummaryLabel = nullptr;
    Label* m_StatusLabel = nullptr;
    TileSetAtlasPreview* m_Preview = nullptr;
    UIElement* m_TerrainSetList = nullptr;
    TileSetTerrainBitEditor* m_TerrainCanvas = nullptr;
    Label* m_TerrainHintLabel = nullptr;
    TileSetCollisionEditor* m_CollisionCanvas = nullptr;
    Label* m_CollisionSelectionLabel = nullptr;
    Dropdown* m_CollisionModeDropdown = nullptr;
    AssetField* m_CollisionMaterialField = nullptr;
    UIElement* m_AnimationRows = nullptr;
    Button* m_AddFrameButton = nullptr;
    // Non-owning; a child of the UI root (or of the panel when there is none).
    // Living on the root means it outlives this panel's removal, so the
    // destructor detaches it and its callbacks are guarded by m_AsyncLifetime.
    ConfirmActionModal* m_ReSliceConfirmModal = nullptr;
    // The grid the visible confirmation describes. The modal is asynchronous and
    // the form stays editable while it is up, so confirming must write the grid
    // the author was shown — not whatever the fields happen to hold later.
    GridSpec m_PendingReSliceGrid;
    // Same ownership and lifetime rules as m_ReSliceConfirmModal.
    ConfirmActionModal* m_DiskConflictModal = nullptr;
    GridSpec m_PendingDiskConflictGrid;

    // Which terrain the canvas paints; -1 for "none armed".
    int16_t m_ActiveTerrainSet = -1;
    int16_t m_ActiveTerrain = -1;
    bool m_TerrainRowsDirty = false;

    // Texture-ready callbacks are queued by the UIManager and can outlive this
    // panel; they capture a weak reference to this token and bail if it expired.
    std::shared_ptr<int> m_AsyncLifetime;
};

} // namespace GameEngine
