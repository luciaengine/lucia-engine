#pragma once

#include "ECS/Systems.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"
#include "Rendering/Core/Handle.h"

#include <array>
#include <utility>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Components
{
struct EZTree;
}

namespace GameEngine::ECS
{
struct EntityHandle;
}

namespace GameEngine::EZTreeECS
{

class EZTreeService;

/// Draws every generated tree as an instance of its variant (EZTreeService). It holds one
/// variant reference and up to three GPU instances per tree, and gives them back when the
/// tree's options change or the tree goes away.
class EZTreeExtractionSystem : public ECS::ISystem
{
public:
    explicit EZTreeExtractionSystem(Engine::Renderer::RenderServices* renderServices = nullptr)
        : m_RenderServices(renderServices)
    {
    }
    ~EZTreeExtractionSystem() override;

    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // World id and entity id.
    using BindingKey = std::pair<uint64, uint32>;
    struct TreeBinding
    {
        uint64 Hash = 0;   // Options hash of the variant this tree holds a reference to; 0 = none.
        uint64 Stamp = 0;  // m_UpdateStamp of the last pass that visited the tree.
        // Branch, leaf, trellis.
        std::array<uint32, 3> Instances{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    };

    struct FrameContext;

    void ExtractTree(FrameContext& frame, ECS::EntityHandle entity, Components::EZTree& tree, bool active);
    static void ReleaseVariantFields(Components::EZTree& tree);
    void ReleaseUnseenTrees(uint64 worldId, EZTreeService& service);
    void RequestImpostors(FrameContext& frame);

    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    FlatMap<BindingKey, TreeBinding> m_Bindings;
    // By variant options hash: the card material index + 1 its parts are linked to; 0 = not linked.
    FlatMap<uint64, uint32> m_ImpostorLinks;
    uint64 m_UpdateStamp = 0;
    // A device rebuild frees the GPU meshes; when the generation moves, the trees add
    // their instances again over the regenerated variants.
    uint64 m_LastDeviceRebuildGeneration = 0;
};

} // namespace GameEngine::EZTreeECS
