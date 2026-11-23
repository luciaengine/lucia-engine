#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <span>

namespace GameEngine
{

/// A project material for one mesh of the tree model, as the scene's MeshRenderer assigns it.
/// A mesh without one uses its imported material.
struct ForestSplatMaterial
{
    uint32 Mesh; // Index in the model's mesh list.
    std::filesystem::path Material; // Relative to the asset root.
};

/// Bakes the splat cells of a tree model into a .geforestsplats file and returns the cell count.
/// The model path is relative to the asset root. Throws std::runtime_error.
uint32 CookForestSplats(const std::filesystem::path& assetRoot, const std::filesystem::path& model,
                        const std::filesystem::path& output, std::span<const ForestSplatMaterial> materials);

} // namespace GameEngine
