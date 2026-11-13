#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "SplineRecipes/FenceEmission.h"
#include "SplineLayout/SpanMiter.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
struct Mesh;
}

namespace GameEngine::SplineRecipes
{

class SplineChunkCommit;

// Vertex and index bytes of cut variants one recipe may hold: the fence
// design's flip condition (section 4c). A castle wall variant is about 4 KB,
// so a castle wall reaches it past about 4 000 distinct joins; a 300 KB stone
// piece past about 50. Past it the remaining mitered pieces draw their pool
// piece, laid as their joins placed them, and the report says so.
inline constexpr uint64 kMaxMiterVariantBytes = 16ull * 1024ull * 1024ull;

// One pool piece a miter variant can be cut from: the model it is drawn from,
// its first submesh (what the fence draws) and the bounds the layout measured
// it by.
struct FenceMiterSource
{
    GUID Model;
    // How the mesh is named to the author.
    std::string Name;
    const Mesh* SourceMesh = nullptr;
    // The registry's content hash of that mesh (MeshGPUEntry::contentHash), so
    // a reimported piece names new variants rather than keeping the old cut.
    uint64 Content = 0;
    SplineLayout::FencePieceBounds Bounds;
};

// A registered variant, ready to draw. The handle is invalid where the piece
// had no mesh to cut.
struct FenceMiterMesh
{
    Rendering::MeshGPUHandle Handle;
    Mathematics::BoundingBox Bounds;
};

// What the fence keeps of each variant it cut, by mesh key, so a rebuild that
// keeps a variant's mesh keeps its bounds and its report without cutting it
// again. Entries the plan no longer names are dropped with their meshes.
struct FenceMiterCut
{
    Mathematics::BoundingBox Bounds;
    uint32 OpenLoops = 0;
    // Vertex and index bytes the variant holds, counted against the budget.
    uint64 Bytes = 0;
};
using FenceMiterCuts = std::unordered_map<Rendering::MeshGPUKey, FenceMiterCut>;

struct FenceMiterVariants
{
    // Parallel to the plan's Variants.
    std::vector<FenceMiterMesh> Meshes;
    // Author-facing lines: a mesh the cut found open, the budget past its end.
    std::vector<std::string> Report;
    // Mitered pieces that draw uncut because the budget ran out before their
    // variant.
    uint32 PiecesPastBudget = 0;
};

// What each pool a variant can be cut from offers the cutter, indexed by its
// active slot: the Span pool, the Crest pool and the Gate pool (a gate is a span
// and is mitered like one).
struct FenceMiterSources
{
    std::span<const FenceMiterSource> Spans;
    std::span<const FenceMiterSource> Crests;
    std::span<const FenceMiterSource> Gates;
};

// Registers every variant the plan names through the recipe's chunk commit and
// releases the ones it no longer names (fence design section 4c). A variant is
// keyed on the recipe entity, its piece's model and content and its quantised
// shape, so
// the equal spans of a uniform arc share one mesh and a rebuild that keeps a
// shape keeps its mesh without cutting it again; a variant is a pure function
// of its key. Each variant is cut from the source its role names. Variants are
// kept in the plan's order until their bytes would pass `byteBudget`; every
// later one is left unregistered.
FenceMiterVariants CommitFenceMiterVariants(Rendering::MeshGPURegistry& registry,
                                            SplineChunkCommit& commit, FenceMiterCuts& cuts,
                                            uint32 entityId,
                                            const SplineLayout::MiterVariantPlan& plan,
                                            const FenceMiterSources& sources,
                                            uint64 byteBudget = kMaxMiterVariantBytes);

// The piece a mitered span or crest draws: its pool piece, drawing the variant's
// mesh within the variant's bounds. It keeps the pool piece's material and its
// model GUID: what reads the GUID (picking first, the navigation and ocean
// depth bakers only) meets the uncut model at the stretched pose, whose extra
// wedge lies inside the neighbouring piece, rather than losing the piece.
FencePoolPiece MiterVariantPiece(const FencePoolPiece& pool, const FenceMiterMesh& variant);

// One number for the registered content of the models a fence cuts variants
// from (their first submesh, as the fence draws it): it changes when any of
// them is reimported, which is what a fence must rebuild on, since nothing
// else it observes changes then. A model not registered yet counts as empty.
uint64 FencePoolContentDigest(const Rendering::MeshGPURegistry& registry,
                              std::span<const GUID> models);

} // namespace GameEngine::SplineRecipes
