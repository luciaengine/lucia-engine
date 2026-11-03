#pragma once

#include "TileMap/TileMapEditContext.h"

#include "ECS/ECS.h"

#include <filesystem>
#include <memory>

namespace GameEngine
{
class ConfirmActionModal;
class EditorPanelManager;
class SceneViewController;
class TilePalettePanel;
class TileSetEditorPanel;
class UIManager;
struct EditorContext;

namespace Editor
{
class UndoRedoService;

/**
 * @brief Owns the tilemap authoring state and wires the surfaces that share it:
 * the palette and tile set editor, and the Scene View paint tool.
 *
 * The application creates one of these and hands it the services it pulls;
 * nothing tilemap-specific lives in the application after that. It also keeps
 * the edit context's paint target in step with the scene selection: selecting
 * an entity that draws a tile layer targets that layer, selecting anything
 * else clears the target. A palette row click retargets within the same
 * selection and stands until the selection moves again. It also adds the
 * Assets panel's "Import as Tilemap" item for Tiled and LDtk source files.
 */
class TileMapEditorIntegration
{
public:
    struct Services
    {
        EditorPanelManager* Panels = nullptr;
        UndoRedoService* UndoRedo = nullptr;
        const EditorContext* Context = nullptr;
        SceneViewController* Scene = nullptr;
        TilePalettePanel* Palette = nullptr;
        TileSetEditorPanel* TileSetEditor = nullptr;
        UIManager* UI = nullptr; // hosts the import confirmation and notice
    };

    void Attach(const Services& services);

    TileMapEditContext& EditContext() { return *m_EditContext; }

private:
    /**
     * Per frame: re-resolves the target only when the selection itself moved.
     * With the palette on screen, landing on a tile layer also arms the Scene
     * View's paint tool: the palette already shows a brush as armed, and a
     * click that then moves the entity instead is the wrong surprise.
     */
    void SyncTargetToSelection(bool paletteVisible);
    /// Puts the Scene View's 2D grid on the target map's cells, or back on decades.
    void SyncGridToTarget();

    /**
     * Imports a Tiled or LDtk file. An import that would replace existing
     * .tileset/.tilemap files asks first and writes nothing until confirmed;
     * the outcome and the importer's warnings end in a notice.
     */
    void ImportSource(const std::filesystem::path& source, bool overwrite);
    /// Builds `modal` on the UI root on first use; null when there is no root.
    ConfirmActionModal* EnsureModal(const char* id);

    std::shared_ptr<TileMapEditContext> m_EditContext = std::make_shared<TileMapEditContext>();
    const EditorContext* m_Context = nullptr;
    SceneViewController* m_Scene = nullptr;
    UIManager* m_UI = nullptr;
    std::filesystem::path m_PendingImport;
    // The selection as last resolved: its size and last entry are enough to
    // tell one selection from the next without reading components every frame.
    size_t m_SyncedSelectionCount = 0;
    ECS::EntityHandle m_SyncedSelectionBack;
    bool m_SyncedOnce = false;
    bool m_SyncedPaletteVisible = false;
};

} // namespace Editor
} // namespace GameEngine
