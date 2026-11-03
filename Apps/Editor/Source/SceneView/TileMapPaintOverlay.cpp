#include "SceneView/TileMapPaintOverlay.h"

#include "Mathematics/MatrixOps.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapEditContext.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <glm/gtc/type_ptr.hpp>

namespace GameEngine
{
namespace Editor::SceneTools
{
namespace
{

constexpr float32 kCursorLineThickness = 2.0f;
constexpr float32 kPaintCursorColor[4] = {0.30f, 0.85f, 1.00f, 1.0f};
constexpr float32 kEraseCursorColor[4] = {1.00f, 0.42f, 0.38f, 1.0f};
constexpr float32 kSelectionLineThickness = 2.0f;
// Warm amber for a committed selection, dimmer for the rectangle still being
// dragged, so the two read apart while a marquee is drawn over an old one.
constexpr float32 kSelectionColor[4] = {1.00f, 0.78f, 0.25f, 1.0f};
constexpr float32 kPendingColor[4] = {1.00f, 0.90f, 0.60f, 0.7f};

// The tint sits under the cursor outline and over the dressed tiles, so it has
// to read as a wash rather than as art.
constexpr float32 kIntGridTintAlpha = 0.45f;
// A stroke over a huge authored region must not turn one frame into a million
// quads; past this the tint simply stops, which is visible and harmless.
constexpr size_t kMaxIntGridTintCells = 20000;

} // namespace

void TileMapCursorGizmo::Render(GizmoRenderContext& context)
{
    if (!Active || CellQuads.empty())
        return;

    const float32* color = Erasing ? kEraseCursorColor : kPaintCursorColor;
    const size_t cellCount = CellQuads.size() / 12; // 4 corners * xyz
    for (size_t cell = 0; cell < cellCount; ++cell)
    {
        const float32* corners = CellQuads.data() + cell * 12;
        for (int32 edge = 0; edge < 4; ++edge)
        {
            const float32* from = corners + edge * 3;
            const float32* to = corners + ((edge + 1) % 4) * 3;
            context.DrawColoredLine({from[0], from[1], from[2]}, {to[0], to[1], to[2]},
                                    {color[0], color[1], color[2], color[3]}, kCursorLineThickness);
        }
    }
}

void TileMapSelectionGizmo::Render(GizmoRenderContext& context)
{
    auto drawOutline = [&context](const std::vector<float32>& quad, const float32* color)
    {
        if (quad.size() < 12)
            return;
        for (int32 edge = 0; edge < 4; ++edge)
        {
            const float32* from = quad.data() + edge * 3;
            const float32* to = quad.data() + ((edge + 1) % 4) * 3;
            context.DrawColoredLine({from[0], from[1], from[2]}, {to[0], to[1], to[2]},
                                    {color[0], color[1], color[2], color[3]}, kSelectionLineThickness);
        }
    };
    drawOutline(SelectionQuad, kSelectionColor);
    drawOutline(PendingQuad, kPendingColor);
}

void TileMapIntGridGizmo::Render(GizmoRenderContext& context)
{
    for (const TintedCell& cell : Cells)
    {
        // Two triangles over the cell quad, wound the same way the corners are.
        const Mathematics::Vector3 triangles[6]{
            {cell.Corners[0], cell.Corners[1], cell.Corners[2]},
            {cell.Corners[3], cell.Corners[4], cell.Corners[5]},
            {cell.Corners[6], cell.Corners[7], cell.Corners[8]},
            {cell.Corners[0], cell.Corners[1], cell.Corners[2]},
            {cell.Corners[6], cell.Corners[7], cell.Corners[8]},
            {cell.Corners[9], cell.Corners[10], cell.Corners[11]}};
        context.DrawTriangles(triangles, 6,
                              {cell.Color[0], cell.Color[1], cell.Color[2], cell.Color[3]});
    }
}

void TileMapPaintOverlay::Clear()
{
    m_Cursor.Active = false;
    m_Cursor.CellQuads.clear();
    m_IntGridTint.Cells.clear();
}

void TileMapPaintOverlay::HideCursor()
{
    m_Cursor.Active = false;
}

void TileMapPaintOverlay::ClearSelection()
{
    m_Selection.SelectionQuad.clear();
    m_Selection.PendingQuad.clear();
}

void TileMapPaintOverlay::ClearPending()
{
    m_Selection.PendingQuad.clear();
}

void TileMapPaintOverlay::UpdateSelection(const TileMapPaintTarget& target,
                                          const TileMapCellRect& selection,
                                          const TileMapCellRect& pending)
{
    BuildRectQuad(target, selection, m_Selection.SelectionQuad);
    BuildRectQuad(target, pending, m_Selection.PendingQuad);
}

void TileMapPaintOverlay::Gather(GizmoCollector& collector)
{
    collector.AddGizmo(&m_IntGridTint);
    collector.AddGizmo(&m_Cursor);
    collector.AddGizmo(&m_Selection);
}

void TileMapPaintOverlay::BuildRectQuad(const TileMapPaintTarget& target, const TileMapCellRect& rect,
                                        std::vector<float32>& outQuad)
{
    outQuad.clear();
    if (!target.Valid || !rect.Valid)
        return;

    const Mathematics::Matrix4x4 localToWorld = Mathematics::Inverse(target.WorldToLocal);
    const auto& m = localToWorld.GetGLM();
    const float32 x0 = static_cast<float32>(rect.MinX) * target.CellWidth;
    const float32 y0 = static_cast<float32>(rect.MinY) * target.CellHeight;
    const float32 x1 = static_cast<float32>(rect.MaxX + 1) * target.CellWidth;
    const float32 y1 = static_cast<float32>(rect.MaxY + 1) * target.CellHeight;
    const float32 corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (const auto& corner : corners)
    {
        const glm::vec4 world = m * glm::vec4(corner[0], corner[1], target.PlaneZ, 1.0f);
        outQuad.push_back(world.x);
        outQuad.push_back(world.y);
        outQuad.push_back(world.z);
    }
}

void TileMapPaintOverlay::UpdateCursor(const TileMapPaintTarget& target,
                                       const TileMapEditContext& editContext, int32 cellX,
                                       int32 cellY, bool erasing)
{
    m_Cursor.CellQuads.clear();
    m_Cursor.Active = target.Valid;
    if (!target.Valid)
        return;

    m_Cursor.Erasing = erasing || editContext.Tool() == TileMapTool::Eraser;

    // The ghost outlines the whole stamp footprint, so a multi-tile selection
    // shows what it will actually cover before the click lands.
    const TileStamp& stamp = editContext.Stamp();
    const bool footprint = !stamp.IsEmpty() && !m_Cursor.Erasing &&
                           !editContext.RandomScatter() &&
                           editContext.Tool() != TileMapTool::Terrain;
    const int32 width = footprint ? stamp.Width : 1;
    const int32 height = footprint ? stamp.Height : 1;

    const Mathematics::Matrix4x4 localToWorld = Mathematics::Inverse(target.WorldToLocal);
    const auto& m = localToWorld.GetGLM();

    for (int32 row = 0; row < height; ++row)
        for (int32 column = 0; column < width; ++column)
        {
            const float32 x0 = static_cast<float32>(cellX + column) * target.CellWidth;
            const float32 y0 = static_cast<float32>(cellY + row) * target.CellHeight;
            const float32 x1 = x0 + target.CellWidth;
            const float32 y1 = y0 + target.CellHeight;
            const float32 corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            for (const auto& corner : corners)
            {
                const glm::vec4 world =
                    m * glm::vec4(corner[0], corner[1], target.PlaneZ, 1.0f);
                m_Cursor.CellQuads.push_back(world.x);
                m_Cursor.CellQuads.push_back(world.y);
                m_Cursor.CellQuads.push_back(world.z);
            }
        }
}

void TileMapPaintOverlay::UpdateIntGridTint(const TileMapPaintTarget& target,
                                            const TileMapEditContext* editContext)
{
    m_IntGridTint.Cells.clear();
    if (!target.Valid || editContext == nullptr || !editContext->ShowIntGridTint())
        return;

    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const TileMap::TileMapData* map = store ? store->GetMap(target.MapGuid) : nullptr;
    const TileMap::TileMapLayerData* targetLayer = map ? map->FindLayer(target.LayerId) : nullptr;
    if (!targetLayer)
        return;

    // Painting a Tiles layer still shows the semantics it is dressed from, so
    // the two halves of the workflow are visible at the same time.
    const TileMap::TileMapLayerData* source =
        targetLayer->Type == TileMap::TileMapLayerType::IntGrid
            ? targetLayer
            : map->FindLayer(targetLayer->Rules.SourceLayerId);
    if (!source || source->Type != TileMap::TileMapLayerType::IntGrid)
        return;

    const Mathematics::Matrix4x4 localToWorld = Mathematics::Inverse(target.WorldToLocal);
    const auto& m = localToWorld.GetGLM();

    for (const TileMap::ChunkCoord chunkCoord : source->SortedIntGridChunkCoords())
    {
        const TileMap::IntGridChunk* chunk = source->FindIntGridChunk(chunkCoord);
        if (!chunk)
            continue;
        for (int32 local = 0; local < TileMap::kChunkCellCount; ++local)
        {
            const uint8 value = chunk->Values[static_cast<size_t>(local)];
            if (value == 0)
                continue;
            if (m_IntGridTint.Cells.size() >= kMaxIntGridTintCells)
                return;

            const TileMap::IntGridValueDef* def = source->FindIntGridValue(value);
            const uint32 rgba = def ? def->ColorRGBA : 0xFFFFFFFFu;
            const int32 cellX = chunkCoord.X * TileMap::kChunkSize + (local % TileMap::kChunkSize);
            const int32 cellY = chunkCoord.Y * TileMap::kChunkSize + (local / TileMap::kChunkSize);

            TileMapIntGridGizmo::TintedCell tinted;
            // 0xRRGGBBAA as authored, matching the terrain swatches; the alpha is
            // the tint's, not the colour's, so the wash stays readable whatever
            // was picked.
            tinted.Color[0] = static_cast<float32>((rgba >> 24) & 0xFFu) / 255.0f;
            tinted.Color[1] = static_cast<float32>((rgba >> 16) & 0xFFu) / 255.0f;
            tinted.Color[2] = static_cast<float32>((rgba >> 8) & 0xFFu) / 255.0f;
            tinted.Color[3] = kIntGridTintAlpha;

            const float32 x0 = static_cast<float32>(cellX) * target.CellWidth;
            const float32 y0 = static_cast<float32>(cellY) * target.CellHeight;
            const float32 corners[4][2] = {{x0, y0},
                                           {x0 + target.CellWidth, y0},
                                           {x0 + target.CellWidth, y0 + target.CellHeight},
                                           {x0, y0 + target.CellHeight}};
            for (int32 corner = 0; corner < 4; ++corner)
            {
                const glm::vec4 world =
                    m * glm::vec4(corners[corner][0], corners[corner][1], target.PlaneZ, 1.0f);
                tinted.Corners[corner * 3 + 0] = world.x;
                tinted.Corners[corner * 3 + 1] = world.y;
                tinted.Corners[corner * 3 + 2] = world.z;
            }
            m_IntGridTint.Cells.push_back(tinted);
        }
    }
}

} // namespace Editor::SceneTools
} // namespace GameEngine
