#pragma once

#include "TileMap/TileLayerEdits.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine
{
namespace Editor
{

/**
 * @brief One command for a layer's existence in both directions: a delete
 * destroys on Do and restores on Undo, a duplicate does the reverse. The
 * payload is identical, only the sense differs.
 *
 * Both directions resolve the world and the runtime store when they run, not
 * when the command is made, so the entry stays valid across the panel rebuilds
 * that happen between an edit and its undo.
 */
class TileLayerExistenceCommand final : public IEditorCommand
{
public:
    TileLayerExistenceCommand(const char* name, bool createsOnDo,
                              TileLayerEdits::LayerRestoreData data);

    const char* GetName() const override { return m_Name; }
    const char* GetTypeName() const override { return "TileLayerExistence"; }
    void Do() override;
    void Undo() override;

private:
    void Create();
    void Destroy();

    const char* m_Name;
    bool m_CreatesOnDo;
    TileLayerEdits::LayerRestoreData m_Data;
};

/**
 * @brief A before/after pair of every layer's editable state. One command
 * shape covers rename, lock, visibility and reorder.
 */
class TileLayerStateCommand final : public IEditorCommand
{
public:
    TileLayerStateCommand(const char* name, TileLayerEdits::LayerStateSnapshot before,
                          TileLayerEdits::LayerStateSnapshot after);

    const char* GetName() const override { return m_Name; }
    const char* GetTypeName() const override { return "TileLayerState"; }
    void Do() override;
    void Undo() override;

private:
    const char* m_Name;
    TileLayerEdits::LayerStateSnapshot m_Before;
    TileLayerEdits::LayerStateSnapshot m_After;
};

} // namespace Editor
} // namespace GameEngine
