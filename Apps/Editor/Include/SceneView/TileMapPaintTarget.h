#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"
#include "Mathematics/Matrix4x4.h"

#include <algorithm>

namespace GameEngine
{
namespace Editor::SceneTools
{

/** Where a stroke writes: the resolved layer and the plane it is drawn on. */
struct TileMapPaintTarget
{
    GUID MapGuid;
    uint32 LayerId = 0;
    Mathematics::Matrix4x4 WorldToLocal;
    float32 CellWidth = 1.0f;
    float32 CellHeight = 1.0f;
    float32 PlaneZ = 0.0f;
    bool Valid = false;
};

/** An inclusive rectangle of cells. */
struct TileMapCellRect
{
    int32 MinX = 0;
    int32 MinY = 0;
    int32 MaxX = 0;
    int32 MaxY = 0;
    bool Valid = false;

    int32 Width() const { return MaxX - MinX + 1; }
    int32 Height() const { return MaxY - MinY + 1; }
    bool Contains(int32 cellX, int32 cellY) const
    {
        return Valid && cellX >= MinX && cellX <= MaxX && cellY >= MinY && cellY <= MaxY;
    }
    static TileMapCellRect FromCorners(int32 x0, int32 y0, int32 x1, int32 y1)
    {
        TileMapCellRect rect;
        rect.MinX = std::min(x0, x1);
        rect.MinY = std::min(y0, y1);
        rect.MaxX = std::max(x0, x1);
        rect.MaxY = std::max(y0, y1);
        rect.Valid = true;
        return rect;
    }
};

} // namespace Editor::SceneTools
} // namespace GameEngine
