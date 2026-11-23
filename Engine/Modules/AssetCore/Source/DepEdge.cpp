#include "AssetCore/DepEdge.h"

namespace GameEngine
{

const char* ToString(DepEdgeKind kind) noexcept
{
    switch (kind)
    {
        case DepEdgeKind::Other:                return "Other";
        case DepEdgeKind::MeshMaterial:         return "MeshMaterial";
        case DepEdgeKind::MaterialTexture:      return "MaterialTexture";
        case DepEdgeKind::MaterialShader:       return "MaterialShader";
        case DepEdgeKind::SceneEntityComponent: return "SceneEntityComponent";
        case DepEdgeKind::PrefabComponent:      return "PrefabComponent";
        case DepEdgeKind::AnimationSkeleton:    return "AnimationSkeleton";
        case DepEdgeKind::UILayoutStyle:        return "UILayoutStyle";
        case DepEdgeKind::UIElementImage:       return "UIElementImage";
        case DepEdgeKind::SceneResource:        return "SceneResource";
        case DepEdgeKind::UIStyleImport:        return "UIStyleImport";
        case DepEdgeKind::ShaderInclude:        return "ShaderInclude";
    }
    return "Unknown";
}

bool IsValidDepEdge(const DepEdge& edge) noexcept
{
    const bool hasGuid = !edge.Target.IsNull();
    const bool hasPath = !edge.TargetPath.empty();
    return hasGuid != hasPath; // exactly one populated
}

std::vector<GUID> DependencyTargets(std::span<const DepEdge> edges)
{
    std::vector<GUID> targets;
    targets.reserve(edges.size());
    for (const DepEdge& edge : edges)
        if (!edge.Target.IsNull())
            targets.push_back(edge.Target);
    return targets;
}

} // namespace GameEngine
