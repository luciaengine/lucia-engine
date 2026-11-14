#pragma once

#include "Assets/ModelAsset.h"

namespace GameEngine::Testing
{
inline constexpr uint32 kClusterLODGridWidth = 49;

inline Mesh MakeClusterLODGrid()
{
    Mesh mesh;
    mesh.Name = "Cluster grid";
    mesh.MaterialIndex = 3;
    for (uint32 row = 0; row < kClusterLODGridWidth; ++row)
    {
        for (uint32 column = 0; column < kClusterLODGridWidth; ++column)
        {
            Vertex vertex{};
            vertex.Position[0] = static_cast<float>(column);
            vertex.Position[2] = static_cast<float>(row);
            vertex.Normal[1] = 1.0f;
            vertex.Tangent[0] = 1.0f;
            vertex.Tangent[3] = 1.0f;
            vertex.TexCoords[0] = static_cast<float>(column) / (kClusterLODGridWidth - 1);
            vertex.TexCoords[1] = static_cast<float>(row) / (kClusterLODGridWidth - 1);
            mesh.Vertices.push_back(vertex);
        }
    }
    for (uint32 row = 0; row + 1 < kClusterLODGridWidth; ++row)
    {
        for (uint32 column = 0; column + 1 < kClusterLODGridWidth; ++column)
        {
            const uint32 first = row * kClusterLODGridWidth + column;
            mesh.Indices.insert(mesh.Indices.end(), {first, first + kClusterLODGridWidth, first + 1,
                first + 1, first + kClusterLODGridWidth, first + kClusterLODGridWidth + 1});
        }
    }
    return mesh;
}

} // namespace GameEngine::Testing
