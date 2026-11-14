#pragma once
// TileMapABI.h - Subsystem-scoped C ABI for tilemap interop
//
// Minimal script surface over an entity's TileMapLayerRef: read/write cells
// (writes route through TileMapRuntimeStore, so render and collision dirty
// tracking fire) and convert between cell coordinates and world space through
// the entity's transform. Asset authoring stays native-only.
//
// Base ABI types/macros (GE_API, GE_CDECL, GE_Result, GE_Handle) are shared
// with ScriptingABI.h; GE_ECS_Entity comes from ECSABI.h.

#include "Scripting/ECSABI.h"

#ifdef __cplusplus
extern "C" {
#endif

// -------- TileMap ABI Versioning --------
#define GE_TILEMAP_ABI_VERSION_ENCODE(major, minor) ((((major) & 0xFFFFu) << 16) | ((minor) & 0xFFFFu))
#define GE_TILEMAP_ABI_VERSION_MAJOR(v)             (((v) >> 16) & 0xFFFFu)
#define GE_TILEMAP_ABI_VERSION_MINOR(v)             ((v) & 0xFFFFu)

// Current TileMap ABI version exposed via GE_TileMap_GetInterface.
static const uint32_t GE_TILEMAP_ABI_VERSION_CURRENT = GE_TILEMAP_ABI_VERSION_ENCODE(1u, 0u);

// Function pointer types for the TileMap interface table. `entity` must carry
// a TileMapLayerRef in the primary world; the cell value is the packed
// TileCell encoding (bits 0..28 tile id, 29 flip H, 30 flip V, 31 transpose).
typedef GE_Result (GE_CDECL *GE_TileMapABI_GetCell_Fn)(GE_ECS_Entity entity, int32_t cellX,
                                                       int32_t cellY, uint32_t* outCell);
typedef GE_Result (GE_CDECL *GE_TileMapABI_SetCell_Fn)(GE_ECS_Entity entity, int32_t cellX,
                                                       int32_t cellY, uint32_t cell);
// World position of the cell's CENTER, through the entity's world transform
// (+ the layer's Z offset).
typedef GE_Result (GE_CDECL *GE_TileMapABI_CellToWorld_Fn)(GE_ECS_Entity entity, int32_t cellX,
                                                           int32_t cellY, float* outWorldX,
                                                           float* outWorldY, float* outWorldZ);
typedef GE_Result (GE_CDECL *GE_TileMapABI_WorldToCell_Fn)(GE_ECS_Entity entity, float worldX,
                                                           float worldY, float worldZ,
                                                           int32_t* outCellX, int32_t* outCellY);

typedef struct GE_TileMap_Interface_v1
{
    uint32_t sizeBytes;  // sizeof(GE_TileMap_Interface_v1)
    uint32_t abiVersion; // GE_TILEMAP_ABI_VERSION_CURRENT

    GE_TileMapABI_GetCell_Fn     GetCell;
    GE_TileMapABI_SetCell_Fn     SetCell;
    GE_TileMapABI_CellToWorld_Fn CellToWorld;
    GE_TileMapABI_WorldToCell_Fn WorldToCell;
} GE_TileMap_Interface_v1;

// Bootstrap entry: returns a pointer to the requested TileMap interface version.
GE_API GE_Result GE_CDECL GE_TileMap_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes);

// Direct exports (routed through GE_TileMap_GetInterface for multi-binding).
GE_API GE_Result GE_CDECL GE_TileMapABI_GetCell(GE_ECS_Entity entity, int32_t cellX, int32_t cellY, uint32_t* outCell);
GE_API GE_Result GE_CDECL GE_TileMapABI_SetCell(GE_ECS_Entity entity, int32_t cellX, int32_t cellY, uint32_t cell);
GE_API GE_Result GE_CDECL GE_TileMapABI_CellToWorld(GE_ECS_Entity entity, int32_t cellX, int32_t cellY, float* outWorldX, float* outWorldY, float* outWorldZ);
GE_API GE_Result GE_CDECL GE_TileMapABI_WorldToCell(GE_ECS_Entity entity, float worldX, float worldY, float worldZ, int32_t* outCellX, int32_t* outCellY);

#ifdef __cplusplus
} // extern "C"
#endif
