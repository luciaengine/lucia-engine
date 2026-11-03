#include "TileMapECS/Systems/TileMapCollisionSystem.h"

#include "Components/TileMap/TileMapCollider.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Logger/Logger.h"
#include "Physics/PhysicsShapes.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/ContactSurfaceTable.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "TileMap/CollisionEdgeMerge.h"
#include "TileMap/TileMapData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace GameEngine
{
namespace TileMapECS
{
namespace
{

// Tiles are flat in the layer plane; bodies are extruded this many cell widths
// along local Z, centered on the plane, so 2D gameplay gets full-depth contacts.
constexpr float32 kColliderDepthInCellWidths = 1.0f;

// Padding on the wake sweep over a rebuilt chunk, so bodies resting just
// outside its exact bounds still notice the ground changing.
constexpr float32 kWakeBoxPaddingMeters = 2.0f;

struct EntityPose
{
    Physics::Vector3 Position;
    Physics::Quaternion Rotation = Physics::Quaternion::Identity();
    Physics::Vector3 Scale{1.0f, 1.0f, 1.0f};
    float32 Matrix[16]; // full column-major local-to-world, for offset points
};

EntityPose PoseFromWorldTransform(const Components::WorldTransform& wt)
{
    Components::Transform t;
    for (int i = 0; i < 16; ++i)
        t.matrix[i] = wt.matrix[i];

    EntityPose pose;
    const auto pos = t.GetPosition();
    const auto rot = t.GetRotation();
    const auto scl = t.GetScale();
    pose.Position = Physics::Vector3(pos.x, pos.y, pos.z);
    {
        const auto& q = rot.GetGLM();
        pose.Rotation = Physics::Quaternion(q.w, q.x, q.y, q.z);
    }
    pose.Scale = Physics::Vector3(scl.x, scl.y, scl.z);
    for (int i = 0; i < 16; ++i)
        pose.Matrix[i] = wt.matrix[i];
    return pose;
}

Physics::Vector3 TransformPoint(const float32* m, float32 x, float32 y, float32 z)
{
    return Physics::Vector3(m[0] * x + m[4] * y + m[8] * z + m[12],
                            m[1] * x + m[5] * y + m[9] * z + m[13],
                            m[2] * x + m[6] * y + m[10] * z + m[14]);
}

bool NearlyEqual(const Physics::Vector3& a, const Physics::Vector3& b)
{
    constexpr float32 kEpsilon = 1.0e-5f;
    return std::fabs(a.x - b.x) <= kEpsilon && std::fabs(a.y - b.y) <= kEpsilon &&
           std::fabs(a.z - b.z) <= kEpsilon;
}

} // namespace

void TileMapCollisionSystem::DestroyChunkBody(Physics::PhysicsWorld& pw, ChunkBody& chunk)
{
    for (Physics::BodyHandle body : chunk.Bodies)
        if (body.IsValid())
            pw.DestroyBody(body);
    for (Physics::ShapeHandle shape : chunk.Shapes)
        if (shape.IsValid())
            pw.DestroyShape(shape);
    chunk.Bodies.clear();
    chunk.Shapes.clear();
}

void TileMapCollisionSystem::DestroyEntityBodies(Physics::PhysicsWorld& pw, EntityState& state)
{
    for (auto& [packed, chunk] : state.Chunks)
        DestroyChunkBody(pw, chunk);
    state.Chunks.clear();
    state.SeenEditVersion = 0;
}

uint32 TileMapCollisionSystem::ChunkBodyCount() const
{
    uint32 count = 0;
    for (const auto& [entity, state] : m_Entities)
        for (const auto& [packed, chunk] : state.Chunks)
            count += static_cast<uint32>(chunk.Bodies.size());
    return count;
}

uint32 TileMapCollisionSystem::OccupiedChunkCount() const
{
    uint32 count = 0;
    for (const auto& [entity, state] : m_Entities)
        count += static_cast<uint32>(state.Chunks.size());
    return count;
}

void TileMapCollisionSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    if (!pw)
    {
        // No physics world: any handles we held died with the previous one.
        m_Entities.clear();
        return;
    }
    const uint64 generation = PhysicsECS::PhysicsWorldService::GetWorldGeneration();
    if (generation != m_PhysicsWorldGeneration)
    {
        m_Entities.clear();
        m_PhysicsWorldGeneration = generation;
    }

    auto* store = TileMapRuntimeStore::TryGet();
    if (!store)
        return;

    static const bool kTrace = std::getenv("GE_TILEMAP_DEBUG") != nullptr;

    // The store's collision ledger is consumed once per (map, layer) per frame
    // and fanned out to every entity sharing the pair — a second consume would
    // see an already-drained queue.
    std::unordered_map<GUID, std::unordered_map<uint32, std::vector<TileMap::ChunkCoord>>>
        consumedDirty;
    auto dirtyFor = [&](const GUID& mapGuid,
                        uint32 layerId) -> const std::vector<TileMap::ChunkCoord>&
    {
        auto& perLayer = consumedDirty[mapGuid];
        auto it = perLayer.find(layerId);
        if (it == perLayer.end())
        {
            m_DirtyScratch.clear();
            store->ConsumeCollisionDirtyChunks(mapGuid, layerId, m_DirtyScratch);
            it = perLayer.emplace(layerId, m_DirtyScratch).first;
        }
        return it->second;
    };

    // Statics are not island members: sleeping dynamics never notice a chunk
    // body appearing or vanishing beneath them, so every rebuild sweeps the
    // chunk's world footprint awake (same contract as the heightfield path).
    auto wakeOverChunk = [&](const EntityPose& pose, const TileMap::TileMapData& data,
                             float32 zOffset, TileMap::ChunkCoord coord)
    {
        const float32 cellW = data.CellWidth;
        const float32 cellH = data.CellHeight;
        const float32 halfDepth = kColliderDepthInCellWidths * cellW * 0.5f;
        const float32 x0 = static_cast<float32>(coord.X) * TileMap::kChunkSize * cellW;
        const float32 y0 = static_cast<float32>(coord.Y) * TileMap::kChunkSize * cellH;
        const float32 x1 = x0 + TileMap::kChunkSize * cellW;
        const float32 y1 = y0 + TileMap::kChunkSize * cellH;

        Physics::AABB box;
        box.min = Physics::Vector3(std::numeric_limits<float32>::max(),
                                   std::numeric_limits<float32>::max(),
                                   std::numeric_limits<float32>::max());
        box.max = Physics::Vector3(std::numeric_limits<float32>::lowest(),
                                   std::numeric_limits<float32>::lowest(),
                                   std::numeric_limits<float32>::lowest());
        for (float32 lx : {x0, x1})
            for (float32 ly : {y0, y1})
                for (float32 lz : {zOffset - halfDepth, zOffset + halfDepth})
                {
                    const Physics::Vector3 world = TransformPoint(pose.Matrix, lx, ly, lz);
                    box.min.x = std::min(box.min.x, world.x);
                    box.min.y = std::min(box.min.y, world.y);
                    box.min.z = std::min(box.min.z, world.z);
                    box.max.x = std::max(box.max.x, world.x);
                    box.max.y = std::max(box.max.y, world.y);
                    box.max.z = std::max(box.max.z, world.z);
                }
        box.min = Physics::Vector3(box.min.x - kWakeBoxPaddingMeters,
                                   box.min.y - kWakeBoxPaddingMeters,
                                   box.min.z - kWakeBoxPaddingMeters);
        box.max = Physics::Vector3(box.max.x + kWakeBoxPaddingMeters,
                                   box.max.y + kWakeBoxPaddingMeters,
                                   box.max.z + kWakeBoxPaddingMeters);
        pw->ActivateBodiesInAABB(box);
    };

    auto rebuildChunk = [&](EntityState& state, const EntityPose& pose,
                            const TileMap::TileMapData& data,
                            const TileMap::TileMapLayerData& layer,
                            const TileMap::TileSetAsset& tileSet,
                            const Components::TileMapCollider& collider, uint64 entityId,
                            TileMap::ChunkCoord coord)
    {
        const uint64 packed = TileMap::PackChunkCoord(coord);
        if (auto it = state.Chunks.find(packed); it != state.Chunks.end())
        {
            DestroyChunkBody(*pw, it->second);
            state.Chunks.erase(it);
        }
        wakeOverChunk(pose, data, state.ZOffset, coord);

        const TileMap::TileMapChunk* chunk = layer.FindChunk(coord);
        if (!chunk)
            return;
        const TileMap::ChunkCollisionShapes shapes =
            TileMap::BuildChunkCollisionShapes(*chunk, tileSet);
        if (shapes.IsEmpty())
            return;

        const float32 cellW = data.CellWidth;
        const float32 cellH = data.CellHeight;
        const float32 halfDepth = kColliderDepthInCellWidths * cellW * 0.5f;

        // One compound per (mode, material). The merge already refuses to fuse
        // cells across that pair, so every emitted shape belongs to exactly one
        // group; the groups become separate bodies because the contact-modify
        // seam snapshots flags and material index per body.
        struct SurfaceGroup
        {
            TileMap::TileCollisionMode Mode = TileMap::TileCollisionMode::Solid;
            GUID Material;
            Physics::CompoundShapeDef Compound;
        };
        std::vector<SurfaceGroup> groups; // first-encountered order, for determinism
        auto groupFor = [&](TileMap::TileCollisionMode mode, const GUID& material) -> SurfaceGroup&
        {
            for (SurfaceGroup& g : groups)
                if (g.Mode == mode && g.Material == material)
                    return g;
            groups.push_back(SurfaceGroup{mode, material, {}});
            return groups.back();
        };

        ChunkBody body;

        for (const TileMap::CollisionRect& rect : shapes.Rects)
        {
            Physics::BoxShapeDef box;
            box.halfExtents = Physics::Vector3((rect.MaxX - rect.MinX) * cellW * 0.5f,
                                               (rect.MaxY - rect.MinY) * cellH * 0.5f, halfDepth);
            const Physics::ShapeHandle shape = pw->CreateShape(box, pose.Scale);
            if (!shape.IsValid())
                continue;
            body.Shapes.push_back(shape);

            Physics::CompoundShapeChild child;
            child.shape = shape;
            child.position =
                Physics::Vector3((rect.MinX + rect.MaxX) * 0.5f * cellW * pose.Scale.x,
                                 (rect.MinY + rect.MaxY) * 0.5f * cellH * pose.Scale.y, 0.0f);
            groupFor(rect.Mode, rect.Material).Compound.children.push_back(child);
        }

        for (const TileMap::CollisionOutline& outline : shapes.Outlines)
        {
            // Extruded prism via convex hull. Jolt convex hulls cannot
            // represent concavity, which is why every outline arrives convex
            // (BuildChunkCollisionShapes splits concave ones).
            Physics::ConvexHullShapeDef hull;
            hull.points.reserve(outline.Points.size() * 2);
            for (const Mathematics::Vector2& p : outline.Points)
            {
                hull.points.push_back(Physics::Vector3(p.x * cellW, p.y * cellH, -halfDepth));
                hull.points.push_back(Physics::Vector3(p.x * cellW, p.y * cellH, halfDepth));
            }
            const Physics::ShapeHandle shape = pw->CreateShape(hull, pose.Scale);
            if (!shape.IsValid())
                continue;
            body.Shapes.push_back(shape);

            Physics::CompoundShapeChild child;
            child.shape = shape;
            groupFor(outline.Mode, outline.Material).Compound.children.push_back(child);
        }

        const Physics::Vector3 chunkOrigin = TransformPoint(
            pose.Matrix, static_cast<float32>(coord.X) * TileMap::kChunkSize * cellW,
            static_cast<float32>(coord.Y) * TileMap::kChunkSize * cellH, state.ZOffset);

        size_t childCount = 0;
        for (SurfaceGroup& group : groups)
        {
            if (group.Compound.children.empty())
                continue;
            childCount += group.Compound.children.size();

            const Physics::ShapeHandle root =
                pw->CreateShape(group.Compound, Physics::Vector3(1.0f, 1.0f, 1.0f));
            if (!root.IsValid())
                continue;
            body.Shapes.push_back(root);

            Physics::BodySettings bs;
            bs.shape = root;
            bs.motionType = Physics::MotionType::Static;
            bs.layer = Physics::Layers::Static;
            bs.friction = collider.Friction;
            bs.restitution = collider.Restitution;
            // PhysicsECS convention: backend userData carries the ECS entity id.
            bs.userData = entityId;
            bs.position = chunkOrigin;
            bs.rotation = pose.Rotation;
            // What the engine's default contact predicate reads back off this
            // body: whether the whole group is one-way, and which authored
            // .physmat its friction/restitution come from.
            bs.contactFlags = group.Mode == TileMap::TileCollisionMode::OneWay
                                  ? PhysicsECS::kContactFlagOneWay
                                  : 0u;
            bs.materialIndex = PhysicsECS::ContactSurfaceTable::ResolveMaterial(group.Material);

            const Physics::BodyHandle handle = pw->CreateBody(bs);
            if (handle.IsValid())
                body.Bodies.push_back(handle);
        }

        if (body.Bodies.empty())
        {
            for (Physics::ShapeHandle shape : body.Shapes)
                pw->DestroyShape(shape);
            return;
        }

        ++m_ChunkRebuildCount;
        if (kTrace)
        {
            Logger::Log::Info("TileMapCollision.Rebuild map={} layer={} chunk=({},{}) shapes={} bodies={} totalRebuilds={}",
                              state.Map.ToString(), state.LayerId, coord.X, coord.Y, childCount,
                              body.Bodies.size(), m_ChunkRebuildCount);
        }
        state.Chunks.emplace(packed, std::move(body));
    };

    std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash> visited;

    world.Query<ECS::Read<Components::TileMapLayerRef>, ECS::Read<Components::TileMapCollider>,
                ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle e, const Components::TileMapLayerRef& layerRef,
                const Components::TileMapCollider& collider, const Components::WorldTransform& wt)
            {
                visited.insert(e);
                EntityState& state = m_Entities[e];

                if (layerRef.TileMap.IsNull() || layerRef.LayerId == 0)
                {
                    DestroyEntityBodies(*pw, state);
                    return;
                }

                const GUID mapGuid = layerRef.TileMap.ToGuid();
                const TileMap::TileMapData* data = store->GetMap(mapGuid);
                const TileMap::TileMapLayerData* layer =
                    data ? data->FindLayer(layerRef.LayerId) : nullptr;
                const TileMap::TileSetAsset* tileSet =
                    layer ? store->GetTileSet(layer->TileSetGuid) : nullptr;
                if (!layer || !tileSet)
                {
                    DestroyEntityBodies(*pw, state);
                    return;
                }

                const EntityPose pose = PoseFromWorldTransform(wt);
                const uint64 editVersion = store->EditVersion(mapGuid);

                // A shrunk edit version means the map was re-decoded from disk
                // (fresh RuntimeMap): chunks may have vanished without a dirty
                // entry, so only a full rebuild is sound.
                const bool identityChanged = state.Map != mapGuid ||
                                             state.LayerId != layerRef.LayerId ||
                                             state.ZOffset != layerRef.ZOffset;
                const bool scaleChanged =
                    state.SeenEditVersion != 0 && !NearlyEqual(pose.Scale, state.BakedScale);
                const bool needFull = identityChanged || scaleChanged ||
                                      state.SeenEditVersion == 0 ||
                                      editVersion < state.SeenEditVersion;

                if (needFull)
                {
                    DestroyEntityBodies(*pw, state);
                    state.Map = mapGuid;
                    state.LayerId = layerRef.LayerId;
                    state.ZOffset = layerRef.ZOffset;
                    state.BakedScale = pose.Scale;
                    // Drain the pair's ledger so a stale entry does not replay
                    // onto the freshly built set next frame.
                    (void)dirtyFor(mapGuid, layerRef.LayerId);
                    for (TileMap::ChunkCoord coord : layer->SortedChunkCoords())
                        rebuildChunk(state, pose, *data, *layer, *tileSet, collider,
                                     static_cast<uint64>(e.id), coord);
                }
                else
                {
                    for (TileMap::ChunkCoord coord : dirtyFor(mapGuid, layerRef.LayerId))
                        rebuildChunk(state, pose, *data, *layer, *tileSet, collider,
                                     static_cast<uint64>(e.id), coord);

                    if (wt.Version != state.SeenTransformVersion)
                    {
                        const float32 cellW = data->CellWidth;
                        const float32 cellH = data->CellHeight;
                        for (auto& [packed, chunk] : state.Chunks)
                        {
                            const TileMap::ChunkCoord coord = TileMap::UnpackChunkCoord(packed);
                            Physics::Transform pose2;
                            pose2.position = TransformPoint(
                                pose.Matrix,
                                static_cast<float32>(coord.X) * TileMap::kChunkSize * cellW,
                                static_cast<float32>(coord.Y) * TileMap::kChunkSize * cellH,
                                state.ZOffset);
                            pose2.rotation = pose.Rotation;
                            for (Physics::BodyHandle handle : chunk.Bodies)
                                pw->SetBodyTransform(handle, pose2,
                                                     Physics::ActivationMode::DontActivate);
                        }
                    }
                }

                state.SeenEditVersion = editVersion;
                state.SeenTransformVersion = wt.Version;
            });

    // Entities that lost the component pair, switched the collider off (the query
    // skips a switched-off collider) or were destroyed drop their bodies.
    for (auto it = m_Entities.begin(); it != m_Entities.end();)
    {
        if (!visited.contains(it->first))
        {
            DestroyEntityBodies(*pw, it->second);
            it = m_Entities.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace TileMapECS
} // namespace GameEngine
