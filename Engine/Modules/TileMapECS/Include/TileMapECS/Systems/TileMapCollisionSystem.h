#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Systems.h"
#include "Physics/PhysicsTypes.h"
#include "TileMap/TileMapChunk.h"

#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Physics
{
class PhysicsWorld;
} // namespace Physics

namespace TileMapECS
{

/**
 * @brief Bakes TileMapCollider entities' layers into per-chunk static Jolt
 * bodies, rebuilt incrementally from the runtime store's collision dirty
 * ledger.
 *
 * Each occupied chunk becomes one static compound body (merged rectangles as
 * boxes, non-square tile polygons as extruded convex hulls) positioned by the
 * entity transform plus the chunk origin. Cell edits rebuild only the touched
 * chunks; emptied chunks lose their body; component/entity removal, layer or
 * map swap, and physics-world restarts tear state down.
 */
class TileMapCollisionSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "TileMapCollisionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    /** Total chunk body (re)builds since construction — rebuild-granularity diagnostics. */
    uint64 ChunkRebuildCount() const { return m_ChunkRebuildCount; }

    /** Chunk bodies currently alive across all tracked entities. */
    uint32 ChunkBodyCount() const;

    /** Chunks currently holding at least one body, across all tracked entities. */
    uint32 OccupiedChunkCount() const;

private:
    // One chunk becomes one body per distinct (collision mode, physics
    // material) group, because the contact-modify seam snapshots its flags and
    // material index per *body*: a chunk mixing one-way and solid tiles, or ice
    // and stone, cannot express that on a single body. The merge already emits
    // shapes homogeneous in that pair, so the split costs no extra shapes.
    struct ChunkBody
    {
        std::vector<Physics::BodyHandle> Bodies;
        std::vector<Physics::ShapeHandle> Shapes; // children + compound roots
    };

    struct EntityState
    {
        GUID Map;
        uint32 LayerId = 0;
        uint64 SeenEditVersion = 0;
        uint32 SeenTransformVersion = 0;
        float32 ZOffset = 0.0f;
        // Entity scale baked into the chunk shapes; a change forces a rebuild
        // (Jolt static shapes cannot rescale in place).
        Physics::Vector3 BakedScale{1.0f, 1.0f, 1.0f};
        std::unordered_map<uint64, ChunkBody> Chunks; // keyed by packed ChunkCoord
    };

    void DestroyChunkBody(Physics::PhysicsWorld& pw, ChunkBody& chunk);
    void DestroyEntityBodies(Physics::PhysicsWorld& pw, EntityState& state);

    std::unordered_map<ECS::EntityHandle, EntityState, ECS::EntityHandleHash> m_Entities;
    uint64 m_PhysicsWorldGeneration = 0;
    uint64 m_ChunkRebuildCount = 0;
    std::vector<TileMap::ChunkCoord> m_DirtyScratch;
};

} // namespace TileMapECS
} // namespace GameEngine
