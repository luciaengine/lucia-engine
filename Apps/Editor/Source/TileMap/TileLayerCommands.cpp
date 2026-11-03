#include "TileMap/TileLayerCommands.h"

#include "Core/Engine.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <utility>

namespace GameEngine
{
namespace Editor
{

namespace
{
struct EditTargets
{
    ECS::World* World = nullptr;
    TileMapECS::TileMapRuntimeStore* Store = nullptr;
    bool Valid() const { return World != nullptr && Store != nullptr; }
};

EditTargets ResolveEditTargets()
{
    return {EngineCore::GetInstance().GetPrimaryWorld(), TileMapECS::TileMapRuntimeStore::TryGet()};
}
} // namespace

TileLayerExistenceCommand::TileLayerExistenceCommand(const char* name, bool createsOnDo,
                                                     TileLayerEdits::LayerRestoreData data)
    : m_Name(name), m_CreatesOnDo(createsOnDo), m_Data(std::move(data))
{
}

void TileLayerExistenceCommand::Do()
{
    if (m_CreatesOnDo)
        Create();
    else
        Destroy();
}

void TileLayerExistenceCommand::Undo()
{
    if (m_CreatesOnDo)
        Destroy();
    else
        Create();
}

void TileLayerExistenceCommand::Create()
{
    const EditTargets targets = ResolveEditTargets();
    if (targets.Valid())
        TileLayerEdits::RestoreLayerFrom(*targets.World, *targets.Store, m_Data);
}

void TileLayerExistenceCommand::Destroy()
{
    const EditTargets targets = ResolveEditTargets();
    if (targets.Valid())
        TileLayerEdits::DestroyLayerAndEntity(*targets.World, *targets.Store, m_Data.Map,
                                              m_Data.Layer.Id);
}

TileLayerStateCommand::TileLayerStateCommand(const char* name,
                                             TileLayerEdits::LayerStateSnapshot before,
                                             TileLayerEdits::LayerStateSnapshot after)
    : m_Name(name), m_Before(std::move(before)), m_After(std::move(after))
{
}

void TileLayerStateCommand::Do()
{
    const EditTargets targets = ResolveEditTargets();
    if (targets.Valid())
        TileLayerEdits::RestoreLayerState(*targets.World, *targets.Store, m_After);
}

void TileLayerStateCommand::Undo()
{
    const EditTargets targets = ResolveEditTargets();
    if (targets.Valid())
        TileLayerEdits::RestoreLayerState(*targets.World, *targets.Store, m_Before);
}

} // namespace Editor
} // namespace GameEngine
