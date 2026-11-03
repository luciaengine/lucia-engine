#include "Panels/HierarchyPanel.h"
#include "Panels/HierarchyCommandIds.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Editor/Entities/SkyboxEntityFactory.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenPlaceholderFactory.h"
#include "Assets/PolyhavenService.h"
#include "Assets/TextureAsset.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Editor/Hierarchy/HierarchyReparentRules.h"
#include "Editor/Hierarchy/HierarchySearch.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PolyhavenPlaceholder.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/DDGIVolume.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneBlueprintInstance.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Editor/Assets/AssetRelativePath.h"
#include "ECS/Components.h"   // Disabled marker component
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h" // ensure template implementations are visible
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Assets/AsyncAssetHelpers.h"
#include "Editor/DragDropPayloads.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "Editor/Entities/EntityDuplicate.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Hierarchy/HierarchyRowActivity.h"
#include "Editor/Hierarchy/HierarchyLightIconTint.h"
#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "Editor/Hierarchy/HierarchyNavigationBar.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Entities/SpriteEntityFactory.h"
#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Sky/SkyEnvironmentCreation.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Mathematics/Types.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/SettingsPanel.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "PlayMode/PlayModeManager.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Terrain/TerrainEntityProvisioning.h"
#include "TerrainECS/TerrainFactory.h"
#include "TerrainECS/TerrainService.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Dropdown.h"
#include "UI/EditorIcons.h"
#include "UI/EditorSearchBars.h"
#include "UI/Interaction/Payload.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/PanelSearchBar.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DeleteEntitiesSelectionCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/MaterialDropOnMeshCommand.h"
#include "UndoRedo/ReparentReorderEntitiesCommand.h"
#include "UndoRedo/TextureDropOnMeshCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "VersionControl/Ui/HierarchyVcsController.h"
#include "Editor/Vcs/VcsStatusUi.h"
#include "Platform/Environment.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <thread>

#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace
{
// Above this many per-Update deltas (created + destroyed entities, or actual reparent
// moves) the per-entity ops lose to a single full Rebuild: each sorted-insert / erase into
// a large sibling list is O(list), so an N-sized delta degrades to O(N^2) vector shifts
// (a 100k bench spawn would memmove gigabytes and hang for seconds). At/under the threshold
// the per-entity path wins; over it, Rebuild is the always-correct fallback (and reseeds all
// bookkeeping). 512 is comfortably above normal editor/gameplay per-frame churn yet far below
// the point where O(N^2) bites.
constexpr std::size_t kIncrementalDeltaMax = 512;

// Row icons are left-aligned in their box, so a letterboxed sprite preview lines
// up with the glyph icons in the rows above and below it.
constexpr float kHierarchyRowIconAlignPercent = 0.0f;

// Kill switch (design §5): GE_HIERARCHY_INCREMENTAL=0 restores the pre-incremental
// full-Refresh Update path exactly. Default ON. Read once per process.
bool HierarchyIncrementalEnabled()
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_HIERARCHY_INCREMENTAL", true);
    return enabled;
}

// RAII: force a bool flag true for the current scope and restore its prior value on exit
// (exception-safe). Used to keep selection-undo suppressed across a programmatic selection
// change / structural refresh even if a callee throws — otherwise a std::bad_alloc mid-refresh
// would leave the flag stuck true and silently stop recording selection-undo for the session.
struct ScopedFlag
{
    bool& m_Flag;
    bool m_Prev;
    explicit ScopedFlag(bool& flag) : m_Flag(flag), m_Prev(flag) { m_Flag = true; }
    ~ScopedFlag() { m_Flag = m_Prev; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;
};

static bool IsDescendantOf(ECS::World& world, ECS::EntityHandle node, ECS::EntityHandle possibleAncestor)
{
    if (!node.IsValid() || !possibleAncestor.IsValid())
        return false;
    if (!world.IsValid(node) || !world.IsValid(possibleAncestor))
        return false;

    ECS::EntityHandle cur = node;
    while (cur.IsValid() && world.IsValid(cur))
    {
        if (cur.id == possibleAncestor.id)
            return true;
        auto* p = world.GetComponent<Components::Parent>(cur);
        if (!p || !p->parent.IsValid() || !world.IsValid(p->parent))
            break;
        cur = p->parent;
    }
    return false;
}

static ECS::EntityHandle GetEffectiveParent(ECS::World& world, ECS::EntityHandle e)
{
    if (!e.IsValid() || !world.IsValid(e))
        return {};
    if (auto* p = world.GetComponent<Components::Parent>(e))
    {
        if (p->parent.IsValid() && world.IsValid(p->parent))
            return p->parent;
    }
    return {};
}

static std::int32_t GetOrderOrDefault(ECS::World& world, ECS::EntityHandle e)
{
    if (!e.IsValid() || !world.IsValid(e))
        return 0;
    if (auto* o = world.GetComponent<Components::HierarchyOrder>(e))
        return o->order;
    return 0;
}

static std::vector<ECS::EntityHandle> CollectAllEntities(ECS::World& world)
{
    std::vector<ECS::EntityHandle> out;
    auto archetypes = world.GetAllArchetypes();
    for (auto* arch : archetypes)
    {
        if (!arch)
            continue;
        auto ents = arch->CollectEntities();
        for (const auto& e : ents)
        {
            if (e.IsValid() && world.IsValid(e))
                out.push_back(e);
        }
    }

    return out;
}

constexpr std::int32_t kDropSiblingOrderStep = 10;
constexpr std::string_view kPolyhavenBookmarkPrefix = "polyhaven:";

void SortByOrderThenId(ECS::World& world, std::vector<ECS::EntityHandle>& entities)
{
    std::sort(entities.begin(), entities.end(), [&world](const ECS::EntityHandle& a, const ECS::EntityHandle& b)
    {
        const std::int32_t oa = GetOrderOrDefault(world, a);
        const std::int32_t ob = GetOrderOrDefault(world, b);
        if (oa != ob)
            return oa < ob;
        return a.id < b.id;
    });
}

// The children of `parent` ({} = the roots) in display order, with `inserted`
// placed at the drop line: before or after `target` when the drop landed
// between rows next to it, otherwise last.
std::vector<ECS::EntityHandle> SiblingsWithDropInsertion(ECS::World& world,
                                                         ECS::EntityHandle parent,
                                                         ECS::EntityHandle target,
                                                         UI::Interaction::DropLocation location,
                                                         const std::vector<ECS::EntityHandle>& inserted)
{
    const auto isInserted = [&inserted](const ECS::EntityHandle& e)
    {
        return std::any_of(inserted.begin(), inserted.end(),
                           [&e](const ECS::EntityHandle& i) { return i.id == e.id; });
    };

    std::vector<ECS::EntityHandle> siblings;
    for (const ECS::EntityHandle e : CollectAllEntities(world))
    {
        const ECS::EntityHandle effectiveParent = GetEffectiveParent(world, e);
        const bool sameParent = parent.IsValid() ? (effectiveParent.IsValid() && effectiveParent.index == parent.index)
                                                 : !effectiveParent.IsValid();
        if (sameParent && !isInserted(e))
            siblings.push_back(e);
    }
    SortByOrderThenId(world, siblings);

    std::size_t insertIndex = siblings.size();
    if (target.IsValid() && (location == UI::Interaction::DropLocation::BeforeItem ||
                             location == UI::Interaction::DropLocation::AfterItem))
    {
        for (std::size_t i = 0; i < siblings.size(); ++i)
        {
            if (siblings[i].id == target.id)
            {
                insertIndex = location == UI::Interaction::DropLocation::AfterItem ? i + 1 : i;
                break;
            }
        }
    }

    siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(insertIndex), inserted.begin(), inserted.end());
    return siblings;
}

// Renumbers `siblings` in list order.
void WriteSiblingOrder(ECS::World& world, const std::vector<ECS::EntityHandle>& siblings)
{
    for (std::size_t i = 0; i < siblings.size(); ++i)
    {
        if (!siblings[i].IsValid() || !world.IsValid(siblings[i]))
            continue;
        Components::HierarchyOrder order{};
        order.order = static_cast<std::int32_t>(i) * kDropSiblingOrderStep;
        world.AddComponentImmediate(siblings[i], order);
    }
}

Editor::ReparentReorderEntitiesCommand::EntityState CaptureReparentState(ECS::World& world, ECS::EntityHandle e)
{
    Editor::ReparentReorderEntitiesCommand::EntityState s{};
    s.entity = e;
    if (auto* p = world.GetComponent<Components::Parent>(e))
    {
        s.hadParent = p->parent.IsValid() && world.IsValid(p->parent);
        s.parent = *p;
    }
    if (auto* o = world.GetComponent<Components::HierarchyOrder>(e))
    {
        s.hadOrder = true;
        s.order = *o;
    }
    if (auto* t = world.GetComponent<Components::Transform>(e))
    {
        s.hadTransform = true;
        s.transform = *t;
    }
    return s;
}

void ParentUnder(ECS::World& world, ECS::EntityHandle child, ECS::EntityHandle parent)
{
    if (!parent.IsValid())
        return;
    Components::Parent p{};
    p.parent = parent;
    world.AddComponentImmediate(child, p);
}

bool IsModelFileExtension(std::string_view ext)
{
    const std::string lower = ToLowerAscii(ext);
    return lower == ".glb" || lower == ".gltf" || lower == ".obj" || lower == ".fbx";
}

// A lens flare source named after the definition file.
ECS::EntityHandle CreateLensFlareEntity(ECS::World& world,
                                        const std::filesystem::path& flarePath,
                                        const GUID& flareGuid,
                                        ECS::EntityHandle parent)
{
    const ECS::EntityHandle flareEntity = world.CreateEntity();
    if (!flareEntity.IsValid())
        return {};

    Components::Name name{};
    const std::string stem = flarePath.stem().string();
    std::strncpy(name.value, stem.c_str(), sizeof(name.value) - 1);
    world.AddComponentImmediate(flareEntity, name);

    Components::Transform transform{};
    transform.SetIdentity();
    world.AddComponentImmediate(flareEntity, transform);

    Components::LensFlareSource source{};
    source.Flare.Set(flareGuid);
    world.AddComponentImmediate(flareEntity, source);

    ParentUnder(world, flareEntity, parent);
    return flareEntity;
}

// An entity that instances the dropped file as a scene blueprint.
ECS::EntityHandle CreateBlueprintInstanceEntity(ECS::World& world,
                                                const std::filesystem::path& absPath,
                                                ECS::EntityHandle parent)
{
    const ECS::EntityHandle e = world.CreateEntity();
    if (!e.IsValid())
        return {};

    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    const std::string stem = absPath.stem().string();
    std::strncpy(nm.value, stem.c_str(), sizeof(nm.value) - 1);
    world.AddComponentImmediate(e, nm);

    Components::SceneBlueprintInstance inst{};
    std::memset(inst.sourcePath, 0, sizeof(inst.sourcePath));
    // Drop payloads carry registry paths, which are folded on
    // case-insensitive platforms while the root carries its own
    // spelling; a lexical subtraction compares case and would
    // persist an absolute path into the scene file.
    std::string relStr = Editor::TryMakeAssetRelativePathString(
        EngineCore::GetInstance().GetAssetManager(), absPath);
    if (relStr.empty())
        relStr = absPath.generic_string();
    std::strncpy(inst.sourcePath, relStr.c_str(), sizeof(inst.sourcePath) - 1);
    world.AddComponentImmediate(e, inst);

    ParentUnder(world, e, parent);
    return e;
}
} // namespace

using Components::MeshRenderer;
using Components::Name;
using Components::Parent;
using Components::Transform;
using ECS::EntityHandle;
using ECS::World;

namespace
{
std::string GetEntitySceneTagString(ECS::World& world, ECS::EntityHandle h)
{
    if (const auto* t = world.GetComponent<Components::SceneEntityTag>(h))
        return std::string(t->View());
    return {};
}

ECS::EntityHandle FindEntityBySceneTag(ECS::World& world, std::string_view tag)
{
    if (tag.empty())
        return {};
    std::vector<ECS::EntityHandle> snap;
    world.GetAliveEntitiesSnapshot(snap);
    for (auto h : snap)
    {
        if (!world.IsValid(h))
            continue;
        if (const auto* t = world.GetComponent<Components::SceneEntityTag>(h))
        {
            if (tag == t->View())
                return h;
        }
    }
    return {};
}
} // namespace

static constexpr TreeId kRootId = std::numeric_limits<TreeId>::max();

namespace
{
constexpr const char* kIconEmptyEntity = "editor:Icons/EmptyEntity.png";
constexpr const char* kIconCamera = "editor:Icons/videocam.png";
constexpr const char* kIconLightDirectional = "editor:Icons/DirectionalLight.png";
constexpr const char* kIconLightSpot = "editor:Icons/SpotLight.png";
constexpr const char* kIconLightAmbient = "editor:Icons/AmbientLight.png";
constexpr const char* kIconLightArea = "editor:Icons/arealight.png";
constexpr const char* kIconPlanet = "editor:Icons/planet.svg";
constexpr const char* kIconParticles = "editor:Icons/sparkles.png";
constexpr const char* kIconSkyEnvironment = "editor:Icons/skyenvironment.png";

bool IsLoadRecentSceneCommand(uint32_t cmd)
{
    return cmd >= HierarchyCommandIds::kCmdLoadRecentSceneBase && cmd < (HierarchyCommandIds::kCmdLoadRecentSceneBase + HierarchyCommandIds::kCmdLoadRecentSceneCount);
}

std::string BuildRecentSceneMenuLabel(const std::filesystem::path& scenePath,
                                      const std::unordered_map<std::string, size_t>& fileNameCounts)
{
    std::string label = scenePath.filename().string();
    if (label.empty())
        label = scenePath.string();

    auto it = fileNameCounts.find(label);
    if (it != fileNameCounts.end() && it->second > 1)
    {
        const std::string parent = scenePath.parent_path().filename().string();
        if (!parent.empty())
            label += " (" + parent + ")";
    }

    std::replace(label.begin(), label.end(), '/', '-');
    return label;
}

inline Name MakeName(const std::string& text)
{
    Name n{};
    std::memset(n.value, 0, sizeof(n.value));
    if (text.empty())
        return n;
    const size_t maxCopy = sizeof(n.value) - 1;
    const size_t toCopy = std::min(maxCopy, text.size());
    std::memcpy(n.value, text.data(), toCopy);
    n.value[toCopy] = '\0';
    return n;
}

inline Transform MakeIdentityTransform()
{
    Transform t{};
    t.SetIdentity();
    return t;
}

// SelectionModel stores ids in an unordered_set; GetSelection() order is not stable. Compare as sets.
inline bool SelectionItemSetsEqual(const std::vector<UI::Interaction::ItemId>& a,
                                   const std::vector<UI::Interaction::ItemId>& b)
{
    if (a.size() != b.size())
        return false;
    std::vector<UI::Interaction::ItemId> sa = a;
    std::vector<UI::Interaction::ItemId> sb = b;
    std::sort(sa.begin(), sa.end());
    std::sort(sb.begin(), sb.end());
    return sa == sb;
}

class CreateEntityCommand final : public Editor::IEditorCommand
{
  public:
    CreateEntityCommand(std::string name,
                        ECS::World* world,
                        Editor::EditorChangeNotifications* notifications,
                        ECS::EntityHandle entity,
                        Transform transform,
                        Name entityName,
                        bool hasParent,
                        Parent parent,
                        bool hasRenderer,
                        MeshRenderer renderer,
                        bool hasCamera,
                        Components::Camera camera,
                        bool hasLight,
                        Components::Light light,
                        bool hasPhysicsBody,
                        Components::PhysicsBody physicsBody,
                        bool hasPhysicsCollider,
                        Components::PhysicsCollider physicsCollider,
                        bool hasBoxShape,
                        Components::BoxColliderShape boxShape,
                        bool hasSphereShape,
                        Components::SphereColliderShape sphereShape,
                        bool hasCapsuleShape,
                        Components::CapsuleColliderShape capsuleShape,
                        Components::HierarchyOrder hierarchyOrder)
        : m_Name(std::move(name)),
          m_World(world),
          m_Notifications(notifications),
          m_Entity(entity),
          m_Transform(transform),
          m_EntityName(entityName),
          m_HasParent(hasParent),
          m_Parent(parent),
          m_HasRenderer(hasRenderer),
          m_Renderer(renderer),
          m_HasCamera(hasCamera),
          m_Camera(camera),
          m_HasLight(hasLight),
          m_Light(light),
          m_HasPhysicsBody(hasPhysicsBody),
          m_PhysicsBody(physicsBody),
          m_HasPhysicsCollider(hasPhysicsCollider),
          m_PhysicsCollider(physicsCollider),
          m_HasBoxShape(hasBoxShape),
          m_BoxShape(boxShape),
          m_HasSphereShape(hasSphereShape),
          m_SphereShape(sphereShape),
          m_HasCapsuleShape(hasCapsuleShape),
          m_CapsuleShape(capsuleShape),
          m_HierarchyOrder(hierarchyOrder)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;

        if (!m_World->ReviveEntityImmediatePreserveHandle(m_Entity))
            return;

        m_World->AddComponentImmediate(m_Entity, m_Transform);
        m_World->AddComponentImmediate(m_Entity, m_EntityName);
        if (m_HasParent)
        {
            m_World->AddComponentImmediate(m_Entity, m_Parent);
        }
        if (m_HasRenderer)
        {
            m_World->AddComponentImmediate(m_Entity, m_Renderer);
        }
        if (m_HasCamera)
        {
            m_World->AddComponentImmediate(m_Entity, m_Camera);
        }
        if (m_HasLight)
        {
            m_World->AddComponentImmediate(m_Entity, m_Light);
        }
        if (m_HasPhysicsBody)
        {
            m_World->AddComponentImmediate(m_Entity, m_PhysicsBody);
        }
        if (m_HasPhysicsCollider)
        {
            m_World->AddComponentImmediate(m_Entity, m_PhysicsCollider);
        }
        if (m_HasBoxShape)
        {
            m_World->AddComponentImmediate(m_Entity, m_BoxShape);
        }
        if (m_HasSphereShape)
        {
            m_World->AddComponentImmediate(m_Entity, m_SphereShape);
        }
        if (m_HasCapsuleShape)
        {
            m_World->AddComponentImmediate(m_Entity, m_CapsuleShape);
        }
        m_World->AddComponentImmediate(m_Entity, m_HierarchyOrder);
        NotifyWorldStructure(m_Notifications, m_World);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;                                // not owned
    Editor::EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_Entity{};

    Transform m_Transform{};
    Name m_EntityName{};
    bool m_HasParent = false;
    Parent m_Parent{};
    bool m_HasRenderer = false;
    MeshRenderer m_Renderer{};
    bool m_HasCamera = false;
    Components::Camera m_Camera{};
    bool m_HasLight = false;
    Components::Light m_Light{};

    bool m_HasPhysicsBody = false;
    Components::PhysicsBody m_PhysicsBody{};
    bool m_HasPhysicsCollider = false;
    Components::PhysicsCollider m_PhysicsCollider{};
    bool m_HasBoxShape = false;
    Components::BoxColliderShape m_BoxShape{};
    bool m_HasSphereShape = false;
    Components::SphereColliderShape m_SphereShape{};
    bool m_HasCapsuleShape = false;
    Components::CapsuleColliderShape m_CapsuleShape{};
    Components::HierarchyOrder m_HierarchyOrder{};
};

class CreateParticleEmitterEntityCommand final : public Editor::IEditorCommand
{
  public:
    CreateParticleEmitterEntityCommand(std::string name,
                                       ECS::World* world,
                                       Editor::EditorChangeNotifications* notifications,
                                       ECS::EntityHandle entity,
                                       Transform transform,
                                       Name entityName,
                                       bool hasParent,
                                       Parent parent,
                                       const Components::ParticleEmitter3D& emitter,
                                       Components::HierarchyOrder hierarchyOrder)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_Transform(transform), m_EntityName(entityName), m_HasParent(hasParent), m_Parent(parent), m_Emitter(emitter), m_HierarchyOrder(hierarchyOrder)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        if (!m_World->ReviveEntityImmediatePreserveHandle(m_Entity))
            return;

        m_World->AddComponentImmediate(m_Entity, m_Transform);
        m_World->AddComponentImmediate(m_Entity, m_EntityName);
        if (m_HasParent)
            m_World->AddComponentImmediate(m_Entity, m_Parent);
        m_World->AddComponentImmediate(m_Entity, m_Emitter);
        m_World->AddComponentImmediate(m_Entity, m_HierarchyOrder);
        NotifyWorldStructure(m_Notifications, m_World);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    Transform m_Transform{};
    Name m_EntityName{};
    bool m_HasParent = false;
    Parent m_Parent{};
    Components::ParticleEmitter3D m_Emitter{};
    Components::HierarchyOrder m_HierarchyOrder{};
};

class CreateReflectionProbeEntityCommand final : public Editor::IEditorCommand
{
  public:
    CreateReflectionProbeEntityCommand(std::string name,
                                       ECS::World* world,
                                       Editor::EditorChangeNotifications* notifications,
                                       ECS::EntityHandle entity,
                                       Transform transform,
                                       Name entityName,
                                       bool hasParent,
                                       Parent parent,
                                       Components::ReflectionProbe probe,
                                       Components::HierarchyOrder hierarchyOrder)
        : m_Name(std::move(name)),
          m_World(world),
          m_Notifications(notifications),
          m_Entity(entity),
          m_Transform(transform),
          m_EntityName(entityName),
          m_HasParent(hasParent),
          m_Parent(parent),
          m_Probe(probe),
          m_HierarchyOrder(hierarchyOrder)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        if (!m_World->ReviveEntityImmediatePreserveHandle(m_Entity))
            return;

        m_World->AddComponentImmediate(m_Entity, m_Transform);
        m_World->AddComponentImmediate(m_Entity, m_EntityName);
        if (m_HasParent)
            m_World->AddComponentImmediate(m_Entity, m_Parent);
        m_World->AddComponentImmediate(m_Entity, m_Probe);
        m_World->AddComponentImmediate(m_Entity, m_HierarchyOrder);
        NotifyWorldStructure(m_Notifications, m_World);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    Transform m_Transform{};
    Name m_EntityName{};
    bool m_HasParent = false;
    Parent m_Parent{};
    Components::ReflectionProbe m_Probe{};
    Components::HierarchyOrder m_HierarchyOrder{};
};

class CreateAsEmptyParentCommand final : public Editor::IEditorCommand
{
  public:
    CreateAsEmptyParentCommand(std::string name,
                               ECS::World* world,
                               Editor::EditorChangeNotifications* notifications,
                               ECS::EntityHandle child,
                               ECS::EntityHandle newParent,
                               bool childHadParent,
                               Parent childParentBefore,
                               Transform childTransformBefore,
                               Name parentName)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Child(child), m_NewParent(newParent), m_ChildHadParent(childHadParent), m_ChildParentBefore(childParentBefore), m_ChildTransformBefore(childTransformBefore), m_ParentName(parentName)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Child.IsValid() || !m_NewParent.IsValid())
            return;

        // Restore child first so it doesn't reference a destroyed parent.
        m_World->AddComponentImmediate(m_Child, m_ChildTransformBefore);
        if (m_ChildHadParent)
        {
            m_World->AddComponentImmediate(m_Child, m_ChildParentBefore);
        }
        else
        {
            m_World->RemoveComponentImmediate<Parent>(m_Child);
        }

        m_World->DestroyEntityImmediatePreserveHandle(m_NewParent);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        if (!m_World || !m_Child.IsValid() || !m_NewParent.IsValid())
            return;

        if (!m_World->ReviveEntityImmediatePreserveHandle(m_NewParent))
            return;

        // New parent local transform is the child's previous local transform.
        m_World->AddComponentImmediate(m_NewParent, m_ChildTransformBefore);
        m_World->AddComponentImmediate(m_NewParent, m_ParentName);
        if (m_ChildHadParent)
        {
            // Parent the new entity under the child's previous parent.
            m_World->AddComponentImmediate(m_NewParent, m_ChildParentBefore);
        }
        else
        {
            // Ensure no Parent component on the inserted parent if original child was root.
            m_World->RemoveComponentImmediate<Parent>(m_NewParent);
        }

        // Reparent child under the new parent and reset its local transform to identity.
        Parent p{};
        p.parent = m_NewParent;
        m_World->AddComponentImmediate(m_Child, p);
        m_World->AddComponentImmediate(m_Child, MakeIdentityTransform());

        NotifyWorldStructure(m_Notifications, m_World);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;                                // not owned
    Editor::EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_Child{};
    ECS::EntityHandle m_NewParent{};
    bool m_ChildHadParent = false;
    Parent m_ChildParentBefore{};
    Transform m_ChildTransformBefore{};
    Name m_ParentName{};
};

} // namespace

// Private provider implementation
class HierarchyPanel::HierarchyDataProvider final : public TreeChangeTrackingProvider
{
  public:
    explicit HierarchyDataProvider(World* world)
        : m_World(world) {}

    void SetWorld(World* world)
    {
        m_World = world;
        Rebuild();
    }

    void SetSortMode(HierarchySortMode mode) { m_SortMode = mode; }
    void SetSortDirection(HierarchySortDirection dir) { m_SortDirection = dir; }
    void Rebuild()
    {
        m_Roots.clear();
        m_Children.clear();
        m_EntityParentKey.clear();
        m_PendingResortParents.clear(); // a full rebuild resorts everything; drop stale deferrals
        if (!m_World)
        {
            m_Labels.clear();
            return;
        }
        // m_Labels is intentionally retained across rebuilds: strings are reused in place
        // below (assign keeps each buffer) and stale entries are pruned at the end.

        // Collect alive entities from the World's canonical metadata.
        // This avoids observing transient archetype-move states and eliminates the need for
        // defensive de-duplication when components are added/removed immediately.
        m_World->GetAliveEntitiesSnapshot(m_SnapshotScratch);

        // Physics world settings live in Settings > Project Settings > Physics; hide here.
        m_SnapshotScratch.erase(
            std::remove_if(m_SnapshotScratch.begin(), m_SnapshotScratch.end(), [this](const EntityHandle& e)
                           { return m_World->GetComponent<Components::PhysicsWorldSettingsComponent>(e) != nullptr; }),
            m_SnapshotScratch.end());

        // Stable pre-sort by TreeId (Encode). The decorate-sort below (SortGroup) applies a
        // total order with an id tie-break, so this only fixes the flatten-path order
        // deterministically; final sibling order is fully determined by SortGroup.
        std::sort(m_SnapshotScratch.begin(), m_SnapshotScratch.end(), [](const EntityHandle& a, const EntityHandle& b)
                  { return Encode(a) < Encode(b); });

        // Build membership set (use the same packed representation as TreeId)
        std::unordered_set<uint32_t> alive;
        alive.reserve(m_SnapshotScratch.size());
        for (auto& e : m_SnapshotScratch)
        {
            alive.insert(static_cast<uint32_t>(Encode(e)));
        }

        // Build adjacency from Parent component
        bool anyParent = false;
        for (auto& e : m_SnapshotScratch)
        {
            ECS::Entity ent(m_World, e);
            if (auto* p = ent.Get<Parent>())
            {
                anyParent = true;

                // Key children by the same packed representation we expose via TreeId
                TreeId parentKey = Encode(p->parent);
                if (p->parent.IsValid() && alive.count(static_cast<uint32_t>(parentKey)))
                {
                    m_Children[static_cast<uint32_t>(parentKey)].push_back(e);
                }
                else
                {
                    m_Roots.push_back(e);
                }
            }
            else
            {
                m_Roots.push_back(e);
            }
        }

        // No Parent anywhere: show every entity as a root (still ordered by HierarchyOrder below).
        if (!anyParent)
        {
            m_Children.clear();
            m_Roots = m_SnapshotScratch;
        }

        // Sibling order: either scene-authored (HierarchyOrder) or overridden by the
        // view-only sort mode configured on the panel. The decorate-sort keys and comparator
        // (KeyOf / KeyLess, via SortGroup) are the SAME ones the incremental sorted-inserts
        // use — comparator parity is the correctness crux, so both paths share one helper.
        SortGroup(m_Roots);
        for (auto& kv : m_Children)
            SortGroup(kv.second);

        // Labels: prefer Name component if present, fall back to entity id. Reuse the existing
        // label strings across rebuilds (assign into the slot keeps its buffer) and prune
        // entries whose entity is no longer alive, so a rebuild does not free and reallocate one
        // string per entity every time.
        for (const EntityHandle& e : m_SnapshotScratch)
            BuildLabel(e);
        for (auto it = m_Labels.begin(); it != m_Labels.end();)
        {
            if (alive.count(static_cast<uint32_t>(it->first)) == 0)
                it = m_Labels.erase(it);
            else
                ++it;
        }

        // Seed the incremental position index from the freshly-built adjacency so the per-entity
        // ops and the identity diff have correct bookkeeping to mutate.
        SeedParentKeyIndex();

        MarkAllChanged();
    }

    // ------------------------------------------------------------------
    // Incremental maintenance (design §2 Tier 1+2). Each op mutates the built
    // adjacency for the touched entity/entities only, ending with a structure/subset
    // change signal so TreeView pays only its O(visible) flat walk — not a full rebuild.
    // ------------------------------------------------------------------

    struct DiffOutcome
    {
        bool AnyDestroyed = false;        // a tracked row vanished → caller revalidates selection
        bool AnyMembershipChange = false; // any entity created OR destroyed this batch
        bool Escalate = false;            // delta too large for per-entity ops → caller full-rebuilds
    };

    // Entity create/destroy authority (there are no entity-level lifecycle events, only
    // per-component ones). Merge-diffs the live entity set against the tracked set — no
    // component reads, no sorting — then applies per-delta ops. Also the swap-generation
    // gap-heal backstop. Nothing is applied when the delta exceeds kIncrementalDeltaMax
    // (F1: per-entity inserts into large sibling lists are O(list), so a mass spawn/despawn
    // would be O(N^2)); the caller full-rebuilds instead.
    DiffOutcome ApplyIdentityDiff()
    {
        DiffOutcome out;
        if (!m_World)
            return out;

        m_World->GetAliveEntitiesSnapshot(m_SnapshotScratch);

        // created = live keys not yet tracked. The hide filter is deferred to ApplyCreated
        // (only new entities need the component read), keeping this pass free of reads.
        m_DiffCreated.clear();
        std::unordered_set<uint32_t> snapKeys;
        snapKeys.reserve(m_SnapshotScratch.size());
        for (const EntityHandle& e : m_SnapshotScratch)
        {
            const uint32_t key = static_cast<uint32_t>(Encode(e));
            snapKeys.insert(key);
            if (m_EntityParentKey.find(key) == m_EntityParentKey.end())
                m_DiffCreated.push_back(e);
        }

        // destroyed = tracked keys no longer live. Removed handles are IDs only — never
        // dereferenced (ApplyDestroyed works purely on provider state).
        m_DiffDestroyed.clear();
        for (const auto& kv : m_EntityParentKey)
            if (snapKeys.find(kv.first) == snapKeys.end())
                m_DiffDestroyed.push_back(Decode(static_cast<TreeId>(kv.first)));

        out.AnyMembershipChange = !m_DiffCreated.empty() || !m_DiffDestroyed.empty();
        out.AnyDestroyed = !m_DiffDestroyed.empty();

        // F1: bail to a full Rebuild for large deltas (mass spawn/despawn). Measure-only up to
        // here (no structures mutated), so nothing to unwind.
        if (m_DiffCreated.size() + m_DiffDestroyed.size() > kIncrementalDeltaMax)
        {
            out.Escalate = true;
            return out;
        }

        for (const EntityHandle& e : m_DiffDestroyed)
            ApplyDestroyed(e);
        for (const EntityHandle& e : m_DiffCreated)
            ApplyCreated(e);
        // Intra-batch parent adoption: a child created before its (also-new) parent was
        // rooted during ApplyCreated because the parent wasn't tracked yet. Now that the
        // whole created set is present, re-home any such child. ApplyReparented no-ops when
        // placement is already correct, so pre-existing correctly-placed creates cost nothing.
        for (const EntityHandle& e : m_DiffCreated)
            ApplyReparented(e);

        return out;
    }

    // C6 (visibility inversion): an entity can be query-visible a frame before its Added
    // event, and the identity diff may have already inserted it — so this is insert-if-absent.
    void ApplyCreated(const EntityHandle& e)
    {
        if (!m_World || !e.IsValid())
            return;
        const uint32_t childKey = static_cast<uint32_t>(Encode(e));
        if (m_EntityParentKey.find(childKey) != m_EntityParentKey.end())
            return; // already tracked (C6 / re-delivered)

        // Same hide filter Rebuild applies.
        if (m_World->GetComponent<Components::PhysicsWorldSettingsComponent>(e) != nullptr)
            return;

        const TreeId parentKey = DesiredParentKey(e);
        if (parentKey == kRootId)
            SortedInsert(m_Roots, e);
        else
            SortedInsert(m_Children[static_cast<uint32_t>(parentKey)], e);
        m_EntityParentKey[childKey] = parentKey;
        BuildLabel(e);
        MarkStructureChanged();
    }

    void ApplyDestroyed(const EntityHandle& e)
    {
        const uint32_t childKey = static_cast<uint32_t>(Encode(e));
        auto itPos = m_EntityParentKey.find(childKey);
        if (itPos == m_EntityParentKey.end())
            return; // not tracked — idempotent
        const TreeId storedParent = itPos->second; // copy: the map may rehash below

        // Remove from its own sibling list.
        RemoveFromSiblingList(storedParent, e);

        // Re-root its children: dangling Parent ⇒ root (matches Rebuild's alive-check). Their
        // Parent components still point at the dead handle — identical to today's post-rebuild
        // state.
        auto childrenIt = m_Children.find(childKey);
        if (childrenIt != m_Children.end())
        {
            std::vector<EntityHandle> orphans = std::move(childrenIt->second);
            m_Children.erase(childrenIt);
            for (const EntityHandle& c : orphans)
            {
                SortedInsert(m_Roots, c);
                m_EntityParentKey[static_cast<uint32_t>(Encode(c))] = kRootId;
            }
        }

        m_EntityParentKey.erase(childKey);
        m_Labels.erase(Encode(e));
        MarkStructureChanged();
    }

    // Idempotent within a window (LifecycleEvents contract): compares the current Parent
    // against tracked placement and no-ops when unchanged — so re-delivered Added/Removed
    // spans and chunk-granular Changed<Parent> over-reports cost nothing. Returns true only
    // when the entity actually moved (the caller counts real moves for the F1 escalation; a
    // no-op is O(1) so over-reports don't count against the threshold).
    bool ApplyReparented(const EntityHandle& e)
    {
        if (!m_World)
            return false;
        const uint32_t childKey = static_cast<uint32_t>(Encode(e));
        auto itPos = m_EntityParentKey.find(childKey);
        if (itPos == m_EntityParentKey.end())
            return false; // destroyed / hidden / never tracked — idempotent no-op
        const TreeId storedParent = itPos->second;
        const TreeId desiredParent = DesiredParentKey(e); // absent Parent ⇒ root (Removed<Parent>)
        if (desiredParent == storedParent)
            return false;

        RemoveFromSiblingList(storedParent, e);
        if (desiredParent == kRootId)
            SortedInsert(m_Roots, e);
        else
            SortedInsert(m_Children[static_cast<uint32_t>(desiredParent)], e);
        m_EntityParentKey[childKey] = desiredParent;
        MarkStructureChanged();
        return true;
    }

    // Tier 1: label update + Subset rebind (no flat rebuild). Under name-sensitive sort modes
    // (Alphabetical / Type) a rename can reorder siblings, so escalate to a one-list resort —
    // but only RECORD the touched list (FlushPendingResorts sorts each once). F2: early-return
    // when the freshly computed label is unchanged, so the chunk-stamp over-report (an archetype
    // move stamps every column of source+dest chunks, so Changed<Name> reports ~a chunk of
    // unrelated entities per unrelated create/destroy) costs one string compare, not a resort.
    void ApplyRenamed(const EntityHandle& e)
    {
        if (!m_World)
            return;
        const uint32_t childKey = static_cast<uint32_t>(Encode(e));
        auto itPos = m_EntityParentKey.find(childKey);
        if (itPos == m_EntityParentKey.end())
            return; // not tracked — idempotent

        const TreeId tid = Encode(e);
        ComputeLabel(e, m_LabelScratch);
        auto lit = m_Labels.find(tid);
        const bool changed = (lit == m_Labels.end()) || (lit->second != m_LabelScratch);
        if (!changed)
            return; // F2: no actual label change (filters the chunk-stamp over-report)
        if (lit != m_Labels.end())
            lit->second = m_LabelScratch;
        else
            m_Labels.emplace(tid, m_LabelScratch);

        if (m_SortMode == HierarchySortMode::Custom)
        {
            MarkChanged(tid); // name-independent order: paint-only rebind
            return;
        }
        // Name-sensitive sort: defer the resort of the touched sibling list (coalesced across all
        // renames this Update by FlushPendingResorts — many renames into one list resort once).
        m_PendingResortParents.insert(itPos->second);
        MarkStructureChanged();
    }

    // Sort each sibling list recorded by ApplyRenamed exactly once. Called by the panel after a
    // rename batch (the Changed<Name> scan and the editor Name-commit path both flush).
    void FlushPendingResorts()
    {
        if (m_PendingResortParents.empty())
            return;
        for (TreeId parentKey : m_PendingResortParents)
        {
            if (parentKey == kRootId)
                SortGroup(m_Roots);
            else if (auto cit = m_Children.find(static_cast<uint32_t>(parentKey)); cit != m_Children.end())
                SortGroup(cit->second);
        }
        m_PendingResortParents.clear();
    }

// Debug-config-only parity check. `!defined(NDEBUG)` catches unoptimized debug builds on every
// compiler (incl. macOS/clang Debug where the MSVC-ism `_DEBUG` is unset); `!defined(GE_DEBUGFAST)`
// then excludes this engine's DebugFast config, which seeds Debug flags (so NDEBUG is unset there
// too) but is the daily-driver perf config that must NOT pay a per-frame Rebuild. Release defines
// NDEBUG and is excluded by the first clause.
#if !defined(NDEBUG) && !defined(GE_DEBUGFAST)
    // Behavioral-parity cross-check (design §4): the incremental result must be
    // byte-equivalent to a fresh Rebuild after every mutation batch. Save the incremental
    // state, rebuild from scratch, compare, then restore the incremental state so latent
    // cross-frame accumulation bugs surface instead of being masked.
    void DebugVerifyAgainstRebuild()
    {
        std::vector<EntityHandle> incRoots = m_Roots;
        std::unordered_map<uint32_t, std::vector<EntityHandle>> incChildren = m_Children;
        std::unordered_map<TreeId, std::string> incLabels = m_Labels;
        std::unordered_map<uint32_t, TreeId> incParentKey = m_EntityParentKey;

        Rebuild(); // authoritative; overwrites members

        const bool ok = (incRoots == m_Roots) && (incChildren == m_Children) && (incLabels == m_Labels);
        if (!ok)
        {
            Logger::Log::Error("[Hierarchy] incremental sync diverged from a fresh Rebuild");
            assert(false && "HierarchyDataProvider incremental sync diverged from Rebuild");
        }

        m_Roots = std::move(incRoots);
        m_Children = std::move(incChildren);
        m_Labels = std::move(incLabels);
        m_EntityParentKey = std::move(incParentKey);
    }
#endif

    // Baseline-only rows, injected by the VCS controller. The provider stores
    // and orders them but knows nothing about version control.
    struct GhostRow
    {
        TreeId Id = 0;
        TreeId ParentId = kRootId;
        std::string Label;
    };

    void SetGhostRows(std::vector<GhostRow> rows)
    {
        if (rows.empty() && m_GhostChildren.empty())
            return;
        m_GhostChildren.clear();
        m_GhostLabels.clear();
        for (GhostRow& row : rows)
        {
            m_GhostChildren[row.ParentId].push_back(row.Id);
            m_GhostLabels.emplace(row.Id, std::move(row.Label));
        }
        MarkStructureChanged();
        MarkAllChanged();
    }

    int GhostChildCount(TreeId parent) const
    {
        const auto it = m_GhostChildren.find(parent);
        return it == m_GhostChildren.end() ? 0 : static_cast<int>(it->second.size());
    }

    // ITreeDataProvider
    int GetRootCount() const override { return 1; }
    TreeId GetRootId(int) const override { return kRootId; }
    int GetChildCount(TreeId parent) const override
    {
        if (parent == kRootId)
            return static_cast<int>(m_Roots.size()) + GhostChildCount(parent);
        if (Editor::IsGhostTreeId(parent))
            return GhostChildCount(parent);
        const auto pid = static_cast<uint32_t>(parent);
        auto it = m_Children.find(pid);
        const int entityCount = it == m_Children.end() ? 0 : static_cast<int>(it->second.size());
        return entityCount + GhostChildCount(parent);
    }
    TreeId GetChildId(TreeId parent, int index) const override
    {
        // Ghost rows sort after the live children of the same parent: a deleted
        // sibling should not displace the entities that are still there.
        const auto ghostAt = [this, parent](int ghostIndex) -> TreeId
        {
            const auto it = m_GhostChildren.find(parent);
            if (it == m_GhostChildren.end() || ghostIndex < 0 ||
                ghostIndex >= static_cast<int>(it->second.size()))
                return 0;
            return it->second[static_cast<size_t>(ghostIndex)];
        };

        if (parent == kRootId)
        {
            const int rootCount = static_cast<int>(m_Roots.size());
            if (index >= 0 && index < rootCount)
                return Encode(m_Roots[static_cast<size_t>(index)]);
            return ghostAt(index - rootCount);
        }
        if (Editor::IsGhostTreeId(parent))
            return ghostAt(index);
        const auto pid = static_cast<uint32_t>(parent);
        auto it = m_Children.find(pid);
        const int entityCount = it == m_Children.end() ? 0 : static_cast<int>(it->second.size());
        if (index >= 0 && index < entityCount)
            return Encode(it->second[static_cast<size_t>(index)]);
        return ghostAt(index - entityCount);
    }
    const char* GetLabel(TreeId id) const override
    {
        if (Editor::IsGhostTreeId(id))
        {
            const auto ghost = m_GhostLabels.find(id);
            return ghost != m_GhostLabels.end() ? ghost->second.c_str() : "";
        }
        auto it = m_Labels.find(id);
        if (it != m_Labels.end())
            return it->second.c_str();
        return "";
    }
    bool IsExpandable(TreeId id) const override
    {
        if (id == kRootId)
            return true;
        if (Editor::IsGhostTreeId(id))
            return GhostChildCount(id) > 0;
        const auto pid = static_cast<uint32_t>(id);
        auto it = m_Children.find(pid);
        if (it != m_Children.end() && !it->second.empty())
            return true;
        return GhostChildCount(id) > 0;
    }

    std::unordered_map<TreeId, std::vector<TreeId>> m_GhostChildren;
    std::unordered_map<TreeId, std::string> m_GhostLabels;

    static TreeId Encode(const EntityHandle& e)
    {
        // Pack [version (upper kEntityVersionBits) | index (lower kEntityIndexBits)] explicitly.
        // This avoids relying on EntityHandle's union layout when round-tripping through TreeId.
        uint32 idx = static_cast<uint32>(e.index) & ECS::kEntityIndexMask;
        uint32 ver = static_cast<uint32>(e.version) & ECS::kEntityVersionMask;
        uint32 raw = idx | (ver << ECS::kEntityIndexBits);
        return static_cast<TreeId>(raw);
    }

    static EntityHandle Decode(TreeId id)
    {
        uint32 raw = static_cast<uint32>(id);
        ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
        ECS::EntityVersion ver = static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
        return EntityHandle(idx, ver);
    }

  private:
    // Decorated sort key for one entity under the active sort mode. Only the field(s) the mode
    // needs are filled; KeyLess reads only those. Shared by Rebuild's decorate-sort AND the
    // incremental sorted-inserts (via KeyOf/KeyLess/SortGroup) — one key helper so the two
    // ordering paths cannot diverge (the correctness crux, design §3).
    struct SortKey
    {
        uint32_t Id = 0;         // unique tie-break (EntityHandle::id) — makes KeyLess a total order
        std::int32_t Order = 0;  // Custom mode
        int TypeRank = 0;        // Type mode
        std::string NameKey;     // lowercased; Alphabetical / Type modes
    };
    struct SortScratchEntry
    {
        EntityHandle Handle{};
        SortKey Key;
    };

    std::int32_t OrderOf(const EntityHandle& h) const
    {
        if (auto* o = m_World->GetComponent<Components::HierarchyOrder>(h))
            return o->order;
        return 0;
    }
    std::string LowerNameOf(const EntityHandle& h) const
    {
        std::string name;
        if (auto* n = m_World->GetComponent<Components::Name>(h); n && n->value[0])
            name = n->View();
        else
            name = std::string("Entity ") + std::to_string(h.id);
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name;
    }
    int TypeRankOf(const EntityHandle& h) const
    {
        // Lower rank sorts first in ascending order. Groups roughly by how "structural" the
        // entity is in the scene.
        if (m_World->GetComponent<Components::Camera>(h))
            return 0;
        if (m_World->GetComponent<Components::Light>(h))
            return 1;
        if (m_World->GetComponent<Components::Skybox>(h))
            return 2;
        if (m_World->GetComponent<Components::Terrain>(h))
            return 3;
        if (m_World->GetComponent<Components::ParticleEmitter3D>(h))
            return 4;
        if (m_World->GetComponent<Components::MeshRenderer>(h))
            return 5;
        return 6; // Empty / other
    }

    // Read one entity's sort key (per active mode) from the ECS. One read per key field.
    SortKey KeyOf(const EntityHandle& e) const
    {
        SortKey k;
        k.Id = e.id;
        switch (m_SortMode)
        {
        case HierarchySortMode::Alphabetical:
            k.NameKey = LowerNameOf(e);
            break;
        case HierarchySortMode::Type:
            k.TypeRank = TypeRankOf(e);
            k.NameKey = LowerNameOf(e);
            break;
        case HierarchySortMode::Custom:
        default:
            k.Order = OrderOf(e);
            break;
        }
        return k;
    }

    // Strict total order matching Rebuild's old `entryLess` exactly, with direction applied.
    bool KeyLess(const SortKey& a, const SortKey& b) const
    {
        return (m_SortDirection == HierarchySortDirection::Descending) ? KeyLessAsc(b, a) : KeyLessAsc(a, b);
    }
    bool KeyLessAsc(const SortKey& a, const SortKey& b) const
    {
        switch (m_SortMode)
        {
        case HierarchySortMode::Alphabetical:
            if (a.NameKey != b.NameKey)
                return a.NameKey < b.NameKey;
            return a.Id < b.Id;
        case HierarchySortMode::Type:
            if (a.TypeRank != b.TypeRank)
                return a.TypeRank < b.TypeRank;
            if (a.NameKey != b.NameKey)
                return a.NameKey < b.NameKey;
            return a.Id < b.Id;
        case HierarchySortMode::Custom:
        default:
            if (a.Order != b.Order)
                return a.Order < b.Order;
            return a.Id < b.Id;
        }
    }

    // Decorate-sort one sibling list in place (Rebuild + rename escalation).
    void SortGroup(std::vector<EntityHandle>& group)
    {
        if (group.size() < 2)
            return;
        m_SortScratch.clear();
        m_SortScratch.reserve(group.size());
        for (const EntityHandle& e : group)
            m_SortScratch.push_back(SortScratchEntry{e, KeyOf(e)});
        std::sort(m_SortScratch.begin(), m_SortScratch.end(),
                  [this](const SortScratchEntry& a, const SortScratchEntry& b)
                  { return KeyLess(a.Key, b.Key); });
        for (size_t i = 0; i < group.size(); ++i)
            group[i] = m_SortScratch[i].Handle;
    }

    // Insert e into a sorted sibling list at the position dictated by KeyLess (the same total
    // order SortGroup produces), so a sequence of inserts equals a full std::sort of the set.
    void SortedInsert(std::vector<EntityHandle>& list, const EntityHandle& e)
    {
        const SortKey key = KeyOf(e);
        auto pos = std::lower_bound(list.begin(), list.end(), key,
                                    [this](const EntityHandle& a, const SortKey& k)
                                    { return KeyLess(KeyOf(a), k); });
        list.insert(pos, e);
    }

    // Remove e from the sibling list identified by its stored parent key; drops an emptied
    // m_Children bucket so the map key-set matches a fresh Rebuild (which never creates empty
    // buckets). Linear find by id: e's sort key may have changed since insertion (an unresorted
    // rename), so a key-based binary search could miss it.
    void RemoveFromSiblingList(TreeId storedParent, const EntityHandle& e)
    {
        if (storedParent == kRootId)
        {
            auto it = std::find(m_Roots.begin(), m_Roots.end(), e);
            if (it != m_Roots.end())
                m_Roots.erase(it);
            return;
        }
        auto cit = m_Children.find(static_cast<uint32_t>(storedParent));
        if (cit == m_Children.end())
            return;
        auto& list = cit->second;
        auto it = std::find(list.begin(), list.end(), e);
        if (it != list.end())
            list.erase(it);
        if (list.empty())
            m_Children.erase(cit);
    }

    // Desired sibling-list key for e's CURRENT Parent: the parent's key if it is a tracked
    // (alive + visible) entity, else kRootId. Absent/invalid/dangling Parent ⇒ root — matches
    // Rebuild's alive-check semantics.
    TreeId DesiredParentKey(const EntityHandle& e) const
    {
        if (auto* p = m_World->GetComponent<Components::Parent>(e); p && p->parent.IsValid())
        {
            const uint32_t pk = static_cast<uint32_t>(Encode(p->parent));
            if (m_EntityParentKey.find(pk) != m_EntityParentKey.end())
                return static_cast<TreeId>(pk);
        }
        return kRootId;
    }

    // Label rule shared by Rebuild/ApplyCreated (unconditional set) and ApplyRenamed (compare
    // first, F2). Name component if present, else the entity-id fallback.
    void ComputeLabel(const EntityHandle& e, std::string& out) const
    {
        if (auto* name = m_World->GetComponent<Components::Name>(e); name && name->value[0])
            out.assign(name->View());
        else
        {
            out.assign("Entity ");
            out += std::to_string(e.id);
        }
    }

    void BuildLabel(const EntityHandle& e) { ComputeLabel(e, m_Labels[Encode(e)]); }

    // (Re)build the child→parent position index from the current adjacency. Called at the end
    // of every full Rebuild so the incremental ops have correct bookkeeping to mutate.
    void SeedParentKeyIndex()
    {
        m_EntityParentKey.clear();
        // Reserve for the total tracked entity count (roots + all children == the hide-filtered
        // snapshot), not m_Children.size() which is the parent-bucket count.
        m_EntityParentKey.reserve(m_SnapshotScratch.size());
        for (const EntityHandle& r : m_Roots)
            m_EntityParentKey[static_cast<uint32_t>(Encode(r))] = kRootId;
        for (const auto& kv : m_Children)
            for (const EntityHandle& c : kv.second)
                m_EntityParentKey[static_cast<uint32_t>(Encode(c))] = static_cast<TreeId>(kv.first);
    }

    World* m_World = nullptr; // not owned
    std::vector<EntityHandle> m_Roots;
    std::unordered_map<uint32_t, std::vector<EntityHandle>> m_Children; // key = parent id
    std::unordered_map<TreeId, std::string> m_Labels;

    // Child key (Encode as uint32) → its sibling list key: kRootId for a root, else the parent's
    // TreeId. Doubles as the tracked-entity membership set for the identity diff and the O(1)
    // position lookup the reparent/destroy ops need. Kept in lockstep with m_Roots/m_Children.
    std::unordered_map<uint32_t, TreeId> m_EntityParentKey;

    std::vector<EntityHandle> m_SnapshotScratch; // reused alive-snapshot buffer (Rebuild + diff)
    std::vector<EntityHandle> m_DiffCreated;     // reused identity-diff delta buffers
    std::vector<EntityHandle> m_DiffDestroyed;
    std::vector<SortScratchEntry> m_SortScratch; // reused across rebuilds and sibling groups
    std::unordered_set<TreeId> m_PendingResortParents; // sibling lists to resort (coalesced renames)
    std::string m_LabelScratch;                  // reused by ApplyRenamed's compare-then-assign
    HierarchySortMode m_SortMode = HierarchySortMode::Custom;
    HierarchySortDirection m_SortDirection = HierarchySortDirection::Ascending;
};

namespace
{
std::mutex& HierarchyPanelRegistryMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_set<HierarchyPanel*>& HierarchyPanelRegistry()
{
    static std::unordered_set<HierarchyPanel*> s;
    return s;
}
} // namespace

HierarchyPanel::HierarchyPanel()
    : DockPanel("Hierarchy")
{
    m_VcsController = std::make_unique<Editor::HierarchyVcsController>(*this);
    {
        std::lock_guard<std::mutex> lock(HierarchyPanelRegistryMutex());
        HierarchyPanelRegistry().insert(this);
    }

    // Used by styling (e.g. for search bar positioning).
    AddClass("hierarchy-panel");
    // The panel's own sheet: the rows' enable-state classes (Editor::HierarchyRowActivity).
    RequestSubtreeStyleAssetPath("UI/panels/HierarchyPanel.css", "editor");

    auto tree = std::make_unique<TreeView>();
    m_Tree = tree.get();
    // Ensure the TreeView participates in flex layout as a fill container
    // (matches Assets panel styling).
    m_Tree->AddClass("tree");
    m_Tree->SetShowRoot(false);
    m_Tree->SetFoldoutTogglesEntireSubtree(true);

    m_Tree->SetOnItemResizeGesture([this](float delta) { ApplyTreeIconSizeFromScroll(delta); });

    // Selection model (needed for .selected CSS styling).
    m_Selection = std::make_unique<UI::Interaction::SelectionModel>();
    m_Tree->SetSelectionModel(m_Selection.get());

    // Provider
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    m_World = world;
    m_Provider = std::make_unique<HierarchyDataProvider>(world);
    m_Tree->SetDataProvider(m_Provider.get());
    // Seed the incremental baselines here too: the constructor binds a world directly (SetWorld
    // isn't guaranteed to run first), and default-0 generations would read as a swap-gen gap > 1
    // on the very first Update and force a spurious full Refresh.
    m_LastWorldStructuralVersion = world ? world->GetStructuralChangeVersion() : 0;
    ArmValueChangeGate();
    ReseedIncrementalBaselines();
    // Defer the initial hierarchy build until after startup/world mutation finishes.
    // Some startup paths clear/reseed the world shortly after UI creation; rebuilding while the
    // ECS is mid-mutation can lead to iterator/pointer invalidation.
    this->PostAction([this]()
                     {
                         if (m_Provider)
                             m_Provider->Rebuild();
                         // Re-seed against the world the provider was just rebuilt from.
                         m_LastWorldStructuralVersion = m_World ? m_World->GetStructuralChangeVersion() : 0;
                         ArmValueChangeGate();
                         ReseedIncrementalBaselines();
                         if (m_Tree)
                             m_Tree->RefreshFromProvider(); });

    // Selection wiring
    m_Tree->SetOnSelectionChanged([this](TreeId id)
                                  {
                                      if (!m_Selection)
                                          return;

                                      // Capture current selection for undo/redo.
                                      UI::Interaction::ItemId anchorAfter = m_Selection->GetAnchor();
                                      std::vector<UI::Interaction::ItemId> selectionAfter = m_Selection->GetSelection();

                                      if (m_Undo && !m_SuppressSelectionUndo)
                                      {
                                          std::vector<UI::Interaction::ItemId> selectionBefore = m_LastSelectionIds;
                                          UI::Interaction::ItemId anchorBefore = m_LastSelectionAnchor;

                                          // Only create an undo command if the selection actually changed.
                                          if (!SelectionItemSetsEqual(selectionBefore, selectionAfter) || anchorBefore != anchorAfter)
                                          {
                                              class HierarchySelectionCommand final : public Editor::IEditorCommand
                                              {
                                                public:
                                                  using NotifyFn = std::function<void(UI::Interaction::ItemId)>;

                                                  HierarchySelectionCommand(UI::Interaction::SelectionModel* selection,
                                                                            std::vector<UI::Interaction::ItemId> beforeIds,
                                                                            UI::Interaction::ItemId beforeAnchor,
                                                                            std::vector<UI::Interaction::ItemId> afterIds,
                                                                            UI::Interaction::ItemId afterAnchor,
                                                                            bool* suppressFlag,
                                                                            NotifyFn notify)
                                                      : m_Selection(selection), m_BeforeIds(std::move(beforeIds)), m_AfterIds(std::move(afterIds)), m_BeforeAnchor(beforeAnchor), m_AfterAnchor(afterAnchor), m_SuppressFlag(suppressFlag), m_Notify(std::move(notify))
                                                  {
                                                  }

                                                  const char* GetName() const override { return "Hierarchy Selection"; }

                                                  void Do() override { Redo(); }

                                                  void Undo() override
                                                  {
                                                      if (!m_Selection || !m_SuppressFlag)
                                                          return;
                                                      *m_SuppressFlag = true;
                                                      m_Selection->SetSelection(m_BeforeIds, m_BeforeAnchor);
                                                      *m_SuppressFlag = false;
                                                      if (m_Notify)
                                                          m_Notify(m_BeforeAnchor);
                                                  }

                                                  void Redo() override
                                                  {
                                                      if (!m_Selection || !m_SuppressFlag)
                                                          return;
                                                      *m_SuppressFlag = true;
                                                      m_Selection->SetSelection(m_AfterIds, m_AfterAnchor);
                                                      *m_SuppressFlag = false;
                                                      if (m_Notify)
                                                          m_Notify(m_AfterAnchor);
                                                  }

                                                private:
                                                  UI::Interaction::SelectionModel* m_Selection = nullptr; // not owned
                                                  std::vector<UI::Interaction::ItemId> m_BeforeIds;
                                                  std::vector<UI::Interaction::ItemId> m_AfterIds;
                                                  UI::Interaction::ItemId m_BeforeAnchor = 0;
                                                  UI::Interaction::ItemId m_AfterAnchor = 0;
                                                  bool* m_SuppressFlag = nullptr; // not owned
                                                  NotifyFn m_Notify;
                                              };

                                              auto notify = [this](UI::Interaction::ItemId /*anchorId*/)
                                              {
                                                  // Undo/redo applied SetSelection already; sync snapshot so the next user
                                                  // selection compares against the correct baseline (otherwise undo appears broken).
                                                  m_SuppressSelectionUndo = true;
                                                  if (m_Selection)
                                                  {
                                                      m_LastSelectionIds = m_Selection->GetSelection();
                                                      m_LastSelectionAnchor = m_Selection->GetAnchor();
                                                  }
                                                  if (m_Tree)
                                                  {
                                                      m_Tree->RefreshFromProvider();
                                                      m_Tree->SyncSelectionVisuals();
                                                  }
                                                  m_SuppressSelectionUndo = false;
                                              };

                                              auto cmd = std::make_unique<HierarchySelectionCommand>(
                                                  m_Selection.get(),
                                                  std::move(selectionBefore),
                                                  anchorBefore,
                                                  selectionAfter,
                                                  anchorAfter,
                                                  &m_SuppressSelectionUndo,
                                                  notify);
                                              m_Undo->CommitAlreadyApplied(std::move(cmd));
                                          }
                                      }

                                      // Update snapshot for next time.
                                      m_LastSelectionIds = std::move(selectionAfter);
                                      m_LastSelectionAnchor = anchorAfter;

                                      // Direct user input path: refresh visuals and inspector immediately.
                                      if (m_Tree)
                                          m_Tree->RefreshFromProvider();
                                      if (!m_SkipOnSelectEntity && id != kRootId)
                                      {
                                          // Prevent the SceneView round-trip from resetting multi-selection.
                                          m_SkipSyncFromSceneView = true;

                                          // Collect all selected entities for multi-edit inspector.
                                          // Use the selection model's true anchor (not the `id` argument, which
                                          // is the row that fired the change — TreeView passes the cursor row
                                          // for Shift+Up/Down range extensions, not the preserved anchor).
                                          // Placing the real anchor first keeps the SceneView round-trip from
                                          // corrupting m_Anchor when SelectEntities rebuilds the selection.
                                          std::vector<ECS::EntityHandle> selectedEntities;
                                          if (m_Selection)
                                          {
                                              const auto allIds = m_Selection->GetSelection();
                                              selectedEntities.reserve(allIds.size());
                                              const UI::Interaction::ItemId trueAnchorId = m_Selection->GetAnchor();
                                              ECS::EntityHandle anchorEntity{};
                                              if (trueAnchorId != 0 && trueAnchorId != kRootId)
                                                  anchorEntity = HierarchyDataProvider::Decode(static_cast<TreeId>(trueAnchorId));
                                              if (anchorEntity.IsValid())
                                                  selectedEntities.push_back(anchorEntity);
                                              for (auto sid : allIds)
                                              {
                                                  if (sid == 0 || sid == kRootId)
                                                      continue;
                                                  ECS::EntityHandle h = HierarchyDataProvider::Decode(static_cast<TreeId>(sid));
                                                  if (h.IsValid() && h != anchorEntity)
                                                      selectedEntities.push_back(h);
                                              }
                                          }

                                          // With a true multi-selection, only fire the multi-entity callback.
                                          // The single-entity path routes through SceneViewController::OnEntityPicked
                                          // which clears m_SelectedEntities and pushes just the anchor — that
                                          // races the multi-entity update and collapses the hierarchy back to
                                          // a single row on the next sync. For multi-select the scene view
                                          // must learn about the full set (via OnEntitiesMarqueeSelected),
                                          // not just the anchor.
                                          const bool multi = selectedEntities.size() > 1;
                                          if (m_OnSelectEntity && !multi)
                                          {
                                              if (id != 0)
                                                  m_OnSelectEntity(HierarchyDataProvider::Decode(static_cast<TreeId>(id)));
                                              else if (selectedEntities.empty())
                                                  m_OnSelectEntity({});
                                          }

                                          // Notify inspector (and scene view, when wired) with all selected entities.
                                          if (m_OnSelectEntities && !selectedEntities.empty())
                                              m_OnSelectEntities(selectedEntities);

                                          m_SkipSyncFromSceneView = false;
                                      }
                                  });

    // Activation: double-click fires frame-in-scene-view.
    m_Tree->SetOnItemActivated([this](TreeId id)
                               {
        if (!m_OnHierarchyItemActivated || id == 0 || id == kRootId)
            return;
        m_OnHierarchyItemActivated(HierarchyDataProvider::Decode(id)); });

    // Per-row styling: add/remove a class when the underlying entity is disabled.
    m_Tree->SetOnRowBound([this](TreeId id, UIElement* row)
                          {
	        if (!row)
	            return;
	        if (!m_World)
	            return;
	        if (id == 0 || id == kRootId)
	            return;

            // A ghost row has no entity behind it; the decode below would yield
            // an invalid handle and every entity affordance would be skipped
            // anyway, so take it here and give the row its own presentation.
            if (Editor::IsGhostTreeId(id))
            {
                m_LayerControls.Hide(*row);
                if (m_VcsController)
                    m_VcsController->DecorateGhostRow(id, *row);
                return;
            }
            row->RemoveClass("hierarchy-ghost-row");

	        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
	        if (!handle.IsValid() || !m_World->IsValid(handle))
	            return;

	        ECS::Entity entity(m_World, handle);
	        if (!entity.IsValid())
	            return;

	        if (m_LockedEntityIds.count(handle.id) ||
                (m_VcsController && m_VcsController->IsSceneLocked()))
	            row->AddClass("entity-locked");
	        else
	            row->RemoveClass("entity-locked");

	        // Distinguish runtime-created entities during play mode.
	        if (m_Context && m_Context->PlayMode && m_Context->PlayMode->IsRuntimeEntity(handle.id))
	            row->AddClass("entity-runtime");
	        else
	            row->RemoveClass("entity-runtime");

	        Editor::ApplyHierarchyEntityIconClasses(m_World, row, handle);

            // Keep the preview icon separate from the title so it can have its own
            // hover state.  TreeView rows are pooled, so create the child once and
            // reuse it across bindings just like the Assets tree does.
            UIElement* titleEl = nullptr;
            UIElement* previewIconEl = nullptr;
            for (const auto& child : row->GetChildren())
            {
                if (child && child->HasClass("tree-title"))
                {
                    titleEl = child.get();
                }
                else if (child && child->HasClass("hierarchy-preview-icon"))
                    previewIconEl = child.get();
            }
            if (m_VcsController)
                m_VcsController->DecorateRow(handle, *row);
            if (titleEl && !previewIconEl)
            {
                auto previewIcon = std::make_unique<UIElement>();
                previewIcon->AddClass("hierarchy-preview-icon");
                previewIcon->AddClass("tree-icon-hit-target");
                previewIcon->SetDisableClipCulling(true);
                previewIcon->RegisterEventHandler(kEventMouseLeave, [](UIEvent& e)
                                                  {
                                                      if (e.CurrentTarget)
                                                          e.CurrentTarget->RemoveClass("tree-icon-hover-preview-suppressed");
                                                  });
                previewIconEl = previewIcon.get();
                row->AddChild(std::move(previewIcon));
            }
            m_RowActivity.Present(*m_World, handle, id, *row);
            if (!row->HasClass("hierarchy-hover-wired"))
            {
                row->AddClass("hierarchy-hover-wired");
                row->RegisterEventHandler(kEventMouseEnter, [this, row](UIEvent&)
                                          {
                    if (!m_OnHoverEntity || !m_Tree)
                        return;
                    const TreeId tid = m_Tree->BoundIdOf(row);
                    if (tid == 0 || tid == kRootId)
                        return;
                    m_OnHoverEntity(HierarchyDataProvider::Decode(tid));
                });
                row->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&)
                                          {
                    if (m_OnHoverEntity)
                        m_OnHoverEntity({});
                });

            }
            if (titleEl)
                m_LayerControls.Present(*m_World, handle, *row, *titleEl, GetLayoutWidth(),
                    [this, row]()
                    {
                        if (!m_Tree)
                            return std::vector<ECS::EntityHandle>{};
                        return ResolveRowActionTargets(HierarchyDataProvider::Decode(m_Tree->BoundIdOf(row)));
                    }, m_Undo, m_ChangeNotifications);
            if (titleEl && previewIconEl)
            {
                titleEl->AddClass("tree-title-no-icon");
                titleEl->RemoveClass("hierarchy-entity-model-thumb");
                UI::Layout::DisableBackgroundOverride(*titleEl);

                const float iconSize = std::max(1.0f, m_LastTreeIconSizePx);
                previewIconEl->Overrides()
                    .Set(Style::Width, StyleLength::Px(iconSize))
                    .Set(Style::Height, StyleLength::Px(iconSize))
                    .Set(Style::MinWidth, StyleLength::Px(iconSize))
                    .Set(Style::MinHeight, StyleLength::Px(iconSize));
                previewIconEl->RemoveClass("hierarchy-entity-model-thumb");
                UI::Layout::DisableBackgroundOverride(*previewIconEl);

                /* The glyph is drawn by this icon, not the row, so the kind
                   class and the shared marker belong here — that is what lets
                   one `.entity-icon.hierarchy-entity-*` rule serve the tree,
                   the Inspector header and the Bookmarks row. The row keeps its
                   own copy of the kind class for IsCssGlyphEntity and the
                   row-level rules. */
                Editor::ApplyHierarchyEntityIconClassesToIcon(m_World, previewIconEl, handle);

                Editor::ApplyHierarchyLightIconTint(*m_World, handle, *previewIconEl);

                // CSS-only rows (empties, terrain, ocean, physics, etc.) never show a
                // thumbnail, so skip the model-GUID subtree scan below. Consult the single
                // authoritative list rather than duplicating it here — a stale copy that
                // omitted 'hierarchy-entity-empty' made every empty row scan its subtree
                // twice per bind (once in ApplyHierarchyEntityIconClasses, once below).
                const bool useCssEntityIcon = Editor::EntityIconIsCssOnly(*row);

                if (!useCssEntityIcon)
                {
                    // Polyhaven placeholder: show the cached thumbnail PNG as the icon.
                    const auto* phPlaceholder = m_World->GetComponent<Components::PolyhavenPlaceholder>(handle);
                    if (phPlaceholder && phPlaceholder->slug[0] != '\0')
                    {
                        std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (std::string(phPlaceholder->Slug()) + ".png");
                        std::error_code ec;
                        if (std::filesystem::exists(thumbPath, ec))
                        {
                            UIManager* mgr = GetOwnerManager();
                            if (mgr)
                            {
                                std::string pathStr = thumbPath.string();
                                GUID thumbGuid = mgr->ResolveBackgroundImagePath(pathStr);
                                if (!thumbGuid.IsNull())
                                {
                                    // Uploads at once when the texture is loaded, otherwise requests it;
                                    // never waits (a first open can still be cooking it).
                                    mgr->EnsureBackgroundTextureUploaded(thumbGuid);

                                    // Fit thumbnail inside the square icon area (1:1), keeping aspect ratio.
                                    BackgroundImageSource source{};
                                    source.Kind = BackgroundImageSource::SourceKind::Path;
                                    source.Value = pathStr;
                                    const float icon = std::max(1.0f, m_LastTreeIconSizePx);
                                    BackgroundSizeValue size{};
                                    size.Mode = BackgroundSizeMode::Explicit;
                                    size.SizeX = icon;
                                    size.SizeXIsPercent = false;
                                    size.SizeY = icon;
                                    size.SizeYIsPercent = false;
                                    BackgroundPositionValue pos{0.0f, true, 50.0f, true};
                                    previewIconEl->AddClass("hierarchy-entity-model-thumb");
                                    previewIconEl->Overrides()
                                        .Set(Style::BackgroundImage, source)
                                        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
                                        .Set(Style::BackgroundSize, size)
                                        .Set(Style::BackgroundPosition, pos)
                                        .Set(Style::BackgroundTint, uint32_t(0xFFFFFFFFu));
                                    ApplyModelThumbColorOverride(previewIconEl);
                                    previewIconEl->MarkDirty(UIElement::VisualDirty);
                                }
                            }
                        }
                    }
                    else if (row->HasClass("hierarchy-entity-animated-sprite") &&
                             m_Context && m_Context->Assets && m_Context->Thumbnails &&
                             !m_Context->UIReplayActive)
                    {
                        // Sprite sheets are useless as previews, so the thumbnail
                        // service crops the entity's current frame out of its
                        // sheet. Falls back to the CSS sprite icon (via class)
                        // while the crop is generating, or when it cannot be.
                        const Editor::SpriteFramesPreviewRequest request =
                            Editor::TryResolveSpriteFramesPreview(m_World, handle, m_Context->Assets);
                        if (!request.AssetPath.empty())
                        {
                            IThumbnailProvider* thumbnails = m_Context->Thumbnails;
                            ApplyGeneratedRowPreview(
                                row, id, previewIconEl,
                                [thumbnails, request](std::function<void(const std::string&)> onReady)
                                {
                                    return thumbnails->GetOrRequestSpriteFrame(
                                        request.AssetPath, request.AnimationName, request.FlipX,
                                        std::move(onReady));
                                });
                        }
                    }
                    else if (row->HasClass("hierarchy-entity-sprite") &&
                             m_Context && m_Context->Assets && m_Context->Thumbnails && !m_Context->UIReplayActive)
                    {
                        const auto path = Editor::TryResolveSpriteAlbedoTexturePath(m_World, handle, m_Context->Assets);
                        if (!path.empty())
                        {
                            const auto uv = Editor::ResolveSpritePreviewUV(m_World, handle, m_Context->Assets);
                            const auto keying = Editor::ResolveSpritePreviewKey(m_World, handle, m_Context->Assets);
                            auto* thumbnails = m_Context->Thumbnails;
                            ApplyGeneratedRowPreview(row, id, previewIconEl,
                                [thumbnails, path, uv, keying](std::function<void(const std::string&)> onReady) {
                                    return thumbnails->GetOrRequestAtlasRegion(path, uv, std::move(onReady), keying);
                                });
                        }
                    }
                    else
                    {
                        // Check entity itself or its children for a model asset (submesh models
                        // have MeshRenderer on children, not the parent).
                        const GUID modelGuid = Editor::FindModelGuidForEntityOrChildren(m_World, handle);
                        if (!modelGuid.IsNull() &&
                            m_Context && m_Context->Thumbnails && !m_Context->UIReplayActive)
                            {
                                // Request by GUID rather than through the path form: an absolute
                                // OPFS path does not match the registry's stored key on web, so
                                // the path round-trip left the icon blank after an import.
                                const std::string engineId =
                                    ModelThumbnailHandler::GetOrRequestByGuid(
                                        modelGuid, /*listStatic*/ true);
                                Editor::ApplyTitleModelThumbnail(
                                    *previewIconEl, engineId, m_LastTreeIconSizePx);
                            }
                    }
                }

                if (!m_CurrentSearchText.empty() && m_SearchMatchIds.count(id) != 0)
                {
                    titleEl->AddClass("search-match");
                    m_SearchHighlightedElements.push_back(titleEl);
                }
                else
                {
                    titleEl->RemoveClass("search-match");
                }
            } });

    // The tree takes every payload the panel takes, plus entity drags for reparenting.
    m_Tree->SetAcceptsPayload([this](UI::Interaction::PayloadTypeId tid)
                              { return AcceptsPayload(tid) ||
                                       tid == UI::Interaction::GetPayloadTypeId<Editor::HierarchyEntityDragPayload>(); });
    m_Tree->SetDragPayloadBuilder([this](TreeId dragSourceId) -> UI::Interaction::DragPayload
                                  {
        if (!m_Selection || !m_World || dragSourceId == 0)
            return {};
        // Drag the whole selection when the grabbed row is part of it; otherwise drag just that row.
        // (The tree selects on mouse-up, so the grabbed row may not be in the committed selection.)
        std::vector<UI::Interaction::ItemId> ids = m_Selection->IsSelected(dragSourceId)
                                                       ? m_Selection->GetSelection()
                                                       : std::vector<UI::Interaction::ItemId>{ dragSourceId };
        if (ids.empty())
            return {};

        // SelectionModel uses an unordered_set; preserve deterministic payload ordering.
        std::sort(ids.begin(), ids.end());

        Editor::HierarchyEntityDragPayload p;
        p.treeIds.reserve(ids.size());
        for (auto id : ids)
        {
            if (id == 0 || id == kRootId)
                continue;
            p.treeIds.push_back(static_cast<std::uint64_t>(id));
        }
        if (p.treeIds.empty())
            return {};

        // Primary item for the coalesced ghost: the grabbed row itself when it's in the payload,
        // otherwise the selection anchor, otherwise the first id.
        TreeId primaryTid = static_cast<TreeId>(p.treeIds[0]);
        if (dragSourceId != kRootId &&
            std::find(p.treeIds.begin(), p.treeIds.end(), static_cast<std::uint64_t>(dragSourceId)) != p.treeIds.end())
        {
            primaryTid = dragSourceId;
        }
        else
        {
            const TreeId anchor = static_cast<TreeId>(m_Selection->GetAnchor());
            if (anchor != 0 && anchor != kRootId && m_Selection->IsSelected(anchor))
                primaryTid = anchor;
        }

        // Display label from primary entity name (best-effort).
        ECS::EntityHandle h = HierarchyDataProvider::Decode(primaryTid);
        std::string_view label;
        if (h.IsValid() && m_World->IsValid(h))
        {
            if (auto* n = m_World->GetComponent<Components::Name>(h))
                label = n->View();
        }
        std::string ghostLabel;
        if (p.treeIds.size() > 1)
            ghostLabel = std::string("Multiple items");
        else if (!label.empty())
            ghostLabel = std::string(label);
        else
            ghostLabel = std::string("Entity");
        p.displayLabel = ghostLabel;

        UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
        out.DisplayLabel = ghostLabel;
        out.GhostIconKind = UI::Interaction::DragGhostIconKind::Entity;

        // Reuse the primary row's resolved preview image so the drag ghost matches
        // the hierarchy row (camera/light/primitive icon or model thumbnail) instead
        // of always falling back to the generic entity cube.
        UIElement* previewIcon = nullptr;
        if (UIElement* row = m_Tree ? m_Tree->FindBoundRow(primaryTid) : nullptr)
            previewIcon = Editor::FindHierarchyPreviewIcon(*row);
        const BackgroundImageStyle* bg =
            previewIcon ? &previewIcon->GetResolvedStyle().Visual.BackgroundImage : nullptr;
        if (bg && bg->HasImage)
        {
            const BackgroundImageSource& source = bg->Source;
            if (source.Kind == BackgroundImageSource::SourceKind::ResourceName)
            {
                out.GhostThumbnailEngineName = "engine:" + source.Value;
            }
            else if (source.Kind == BackgroundImageSource::SourceKind::Path)
            {
                // Preserve the editor source alias; without it a project asset with the
                // same relative path wins (or resolution fails on a fresh registry).
                // File/model thumbnails stay on the normal path-resolution lane.
                out.GhostThumbnailEngineName = source.SourceAlias == "editor"
                                                   ? "editor:" + source.Value
                                                   : "file:" + source.Value;
            }
        }
        return out; });

    // Select on mouse-UP (not down): lets an entity be dragged from the Hierarchy onto an inspector
    // EntityField without the press re-selecting it and swapping the inspector mid-drag. A drag never
    // changes selection; plain clicks commit on release. Shift/Ctrl/RMB/keyboard stay immediate.
    m_Tree->SetSelectOnMouseUp(true);
    m_Tree->SetOnCanDrop([this](const UI::Interaction::DropRequest& req) { return CanDrop(req); });
    m_Tree->SetOnPerformDrop([this](const UI::Interaction::DropRequest& req) { PerformDrop(req); });

    // Icon click: toggle the entity's Enabled/Disabled state and update the row class.
    m_Tree->SetOnIconClicked([this](TreeId id, UIElement* row)
                             {
	        if (!m_World)
	            return;
	        if (id == 0 || id == kRootId)
	            return;

	        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
	        if (!handle.IsValid() || !m_World->IsValid(handle))
	            return;

	        // A selected row applies the toggle to the complete selection: each selected
	        // entity's own state, its descendants following through the hierarchy pass.
	        const bool enabled = !ECS::Entity(m_World, handle).IsEnabled();
	        Editor::CommitEntityEnabledToggle(*m_World, m_Undo, m_ChangeNotifications,
	                                          ResolveRowActionTargets(handle), enabled);

	        if (row)
	            m_RowActivity.Present(*m_World, handle, id, *row);

	        if (m_Tree)
	            m_Tree->RefreshFromProvider(); });

    // Icon state check: returns true if entity is enabled, false if disabled
    m_Tree->SetOnIconStateCheck([this](TreeId id) -> bool
                                {
	        if (!m_World)
	            return false;
	        if (id == 0 || id == kRootId)
	            return false;

	        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
	        if (!handle.IsValid() || !m_World->IsValid(handle))
	            return false;

	        // Return true if enabled (no Disabled component), false if disabled
	        return m_World->GetComponent<ECS::Disabled>(handle) == nullptr; });

    // Icon state set: sets entity to specific enabled/disabled state
    m_Tree->SetOnIconStateSet([this](TreeId id, bool enabled, UIElement* row)
                              {
	        if (!m_World)
	            return;
	        if (id == 0 || id == kRootId)
	            return;

	        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
	        if (!handle.IsValid() || !m_World->IsValid(handle))
	            return;

	        Editor::CommitEntityEnabledToggle(*m_World, m_Undo, m_ChangeNotifications,
	                                          ResolveRowActionTargets(handle), enabled);

	        if (row)
	            m_RowActivity.Present(*m_World, handle, id, *row);

	        if (m_Tree)
	            m_Tree->RefreshFromProvider(); });

    // Lock icon: toggle locked state for the entity
    m_Tree->SetOnLockClicked([this](TreeId id, UIElement* row)
                             {
        if (!m_World) return;
        if (id == 0 || id == kRootId) return;

        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
        if (!handle.IsValid() || !m_World->IsValid(handle)) return;

        const std::uint32_t entityId = handle.id;
        if (m_LockedEntityIds.count(entityId))
            m_LockedEntityIds.erase(entityId);
        else
            m_LockedEntityIds.insert(entityId);

        const bool locked = m_LockedEntityIds.count(entityId) != 0;
        if (row)
        {
            if (locked)
                row->AddClass("entity-locked");
            else
                row->RemoveClass("entity-locked");
        } });

    // Lock state check: returns true if the entity is locked
    m_Tree->SetOnLockStateCheck([this](TreeId id) -> bool
                                {
        if (!m_World) return false;
        if (id == 0 || id == kRootId) return false;

        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
        if (!handle.IsValid() || !m_World->IsValid(handle)) return false;

        return m_LockedEntityIds.count(handle.id) != 0 ||
               (m_VcsController && m_VcsController->IsSceneLocked()); });

    // Lock state set: applied during drag-to-lock
    m_Tree->SetOnLockStateSet([this](TreeId id, bool locked, UIElement* row)
                              {
        if (!m_World) return;
        if (id == 0 || id == kRootId) return;

        ECS::EntityHandle handle = HierarchyDataProvider::Decode(id);
        if (!handle.IsValid() || !m_World->IsValid(handle)) return;

        const std::uint32_t entityId = handle.id;
        if (locked)
            m_LockedEntityIds.insert(entityId);
        else
            m_LockedEntityIds.erase(entityId);

        if (row)
        {
            if (locked)
                row->AddClass("entity-locked");
            else
                row->RemoveClass("entity-locked");
        } });

    // Context menu wiring (TreeView now emits RMB callbacks)
    m_ShowContextMenu = [this](TreeId id, float x, float y)
                             {
        if (!m_Window)
            return;

        if (m_VcsController && Editor::IsGhostTreeId(id))
        {
            m_VcsController->ShowGhostContextMenu(id, x, y);
            return;
        }

        if (m_VcsController && m_VcsController->IsVcsRowControlPoint(x, y))
        {
            m_VcsController->ShowLockContextMenu(id, x, y);
            return;
        }

        if (!m_ContextMenu)
        {
            m_ContextMenu = CreateContextMenu();
            if (m_ContextMenu)
            {
                m_ContextMenu->SetCommandHandler([this](uint32_t cmd)
                                                 {
                    if (HandleCommand(cmd))
                        return;

                    uint64_t dom = 0;
                    std::string method;
                    if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(cmd, dom, method) && !method.empty())
                    {
                        try
                        {
                            auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
                            int32_t out = 0;
                            (void)clr.InvokeInDomain(dom, method.c_str(), (uint32_t)method.size(), &out);
                        }
                        catch (...)
                        {
                        }
                    } });
            }
        }

        if (!m_ContextMenu)
            return;

        // Capture context target for command handler.
        m_ContextTargetEntity = {};
        if (id != 0 && id != kRootId)
        {
            ECS::EntityHandle e = HierarchyDataProvider::Decode(id);
            if (m_World && e.IsValid() && m_World->IsValid(e))
            {
                m_ContextTargetEntity = e;
            }
        }

        const uint32_t mask =
            (id == 0 || id == kRootId) ? static_cast<uint32_t>(Editor::EditorContextMenuTarget::HierarchyEmpty)
                                       : static_cast<uint32_t>(Editor::EditorContextMenuTarget::HierarchyItem);

        auto scriptItems = Editor::ScriptMenuRegistry::Get().GetContextItems(mask);

        ContextMenuBuilder builder;
        // Built-in Create actions
        builder.AddItem("Create", 0, MenuItemFlag_None, -100, EditorIcons::kPlus);
        builder.AddItem("Create/Empty Entity", HierarchyCommandIds::kCmdCreateEmptyEntity, MenuItemFlag_None, -100, kIconEmptyEntity);
        if (mask == static_cast<uint32_t>(Editor::EditorContextMenuTarget::HierarchyItem))
        {
            builder.AddItem("Create/As Empty Parent", HierarchyCommandIds::kCmdCreateAsEmptyParent, MenuItemFlag_None, -99, kIconEmptyEntity);
            builder.AddItem("Duplicate", HierarchyCommandIds::kCmdDuplicate, MenuItemFlag_None, -50, EditorIcons::kCopy);

            if (m_OnAddEntitiesToBookmarks)
                builder.AddItem("Add to Bookmarks", HierarchyCommandIds::kCmdAddToBookmarks, MenuItemFlag_None, -48, EditorIcons::kBookmark);

            // "Show Mesh Location" — only when the target entity (or one of its
            // children) references a model asset we can locate on disk.
            if (m_PingAsset && m_ContextTargetEntity.IsValid() && m_World && m_World->IsValid(m_ContextTargetEntity)
                && m_Context && m_Context->Assets)
            {
                const GUID modelGuid = Editor::FindModelGuidForEntityOrChildren(m_World, m_ContextTargetEntity);
                if (!modelGuid.IsNull())
                {
                    AssetMetadata meta;
                    if (m_Context->Assets->GetRegistry().TryGetAssetMetadata(modelGuid, meta) && !meta.Path.empty())
                    {
                        builder.AddItem("Show Mesh Location", HierarchyCommandIds::kCmdShowMeshLocation, MenuItemFlag_None, -40, EditorIcons::kEye);
                    }
                }
            }
        }
        builder.AddItem("Create/Shape", 0, MenuItemFlag_None, -98, EditorIcons::kCube);
        builder.AddItem("Create/Shape/Cube", HierarchyCommandIds::kCmdCreateShapeCube, MenuItemFlag_None, -98, EditorIcons::kCube);
        builder.AddItem("Create/Shape/Sphere", HierarchyCommandIds::kCmdCreateShapeSphere, MenuItemFlag_None, -97, EditorIcons::kSphere);
        builder.AddItem("Create/Shape/Capsule", HierarchyCommandIds::kCmdCreateShapeCapsule, MenuItemFlag_None, -96, EditorIcons::kCapsule);
        builder.AddItem("Create/Shape/Plane", HierarchyCommandIds::kCmdCreateShapePlane, MenuItemFlag_None, -95, EditorIcons::kPlane);
        builder.AddItem("Create/Light", 0, MenuItemFlag_None, -93, EditorIcons::kLight);
        builder.AddItem("Create/Light/Directional", HierarchyCommandIds::kCmdCreateLightDirectional, MenuItemFlag_None, -93, kIconLightDirectional);
        builder.AddItem("Create/Light/Point", HierarchyCommandIds::kCmdCreateLightPoint, MenuItemFlag_None, -92, EditorIcons::kLight);
        builder.AddItem("Create/Light/Spot", HierarchyCommandIds::kCmdCreateLightSpot, MenuItemFlag_None, -91, kIconLightSpot);
        builder.AddItem("Create/Light/Area", HierarchyCommandIds::kCmdCreateLightArea, MenuItemFlag_None, -90, kIconLightArea);
        builder.AddItem("Create/Light/Ambient", HierarchyCommandIds::kCmdCreateLightAmbient, MenuItemFlag_None, -88, kIconLightAmbient);
        builder.AddItem("Create/Terrain", 0, MenuItemFlag_None, -87, EditorIcons::kTerrain);
        builder.AddItem("Create/Terrain/Terrain (512 m)", HierarchyCommandIds::kCmdCreateTerrain, MenuItemFlag_None, -87, EditorIcons::kTerrain);
        builder.AddItem("Create/Terrain/Terrain \xe2\x80\x94 Large (4 km)", HierarchyCommandIds::kCmdCreateTerrainLarge, MenuItemFlag_None, -87, EditorIcons::kTerrain);
        builder.AddItem("Create/Terrain/Planet (5 km)", HierarchyCommandIds::kCmdCreatePlanet5km, MenuItemFlag_None, -86, kIconPlanet);
        builder.AddItem("Create/Terrain/Planet (50 km)", HierarchyCommandIds::kCmdCreatePlanet50km, MenuItemFlag_None, -86, kIconPlanet);
        builder.AddItem("Create/Camera", HierarchyCommandIds::kCmdCreateCamera, MenuItemFlag_None, -86, kIconCamera);
        builder.AddItem("Create/Ocean", HierarchyCommandIds::kCmdCreateOcean, MenuItemFlag_None, -85, EditorIcons::kOcean);
        builder.AddItem("Create/Sky Environment", HierarchyCommandIds::kCmdCreateSkyEnvironment, MenuItemFlag_None, -85, kIconSkyEnvironment);
        builder.AddItem("Create/Reflection Probe", HierarchyCommandIds::kCmdCreateReflectionProbe, MenuItemFlag_None, -84, EditorIcons::kProbe);
        builder.AddItem("Create/Post Process Volume", HierarchyCommandIds::kCmdCreatePostProcessVolume, MenuItemFlag_None, -84, EditorIcons::kColorFilter);
        builder.AddItem("Create/DDGI Volume", HierarchyCommandIds::kCmdCreateDDGIVolume, MenuItemFlag_None, -84, EditorIcons::kDDGIVolume);
        builder.AddItem("Create/Wind Volume", HierarchyCommandIds::kCmdCreateWindVolume, MenuItemFlag_None, -83, EditorIcons::kWind);
        builder.AddItem("Create/Particle Emitter", HierarchyCommandIds::kCmdCreateParticleEmitter, MenuItemFlag_None, -82, kIconParticles);

        Editor::HierarchyContextMenuContext pluginMenuCtx{};
        pluginMenuCtx.World = m_World;
        pluginMenuCtx.TargetEntity = m_ContextTargetEntity;
        pluginMenuCtx.HasTargetEntity = m_ContextTargetEntity.IsValid() && m_World && m_World->IsValid(m_ContextTargetEntity);
        pluginMenuCtx.EditorCtx = m_Context;
        Editor::EditorPluginRegistry::Get().BuildHierarchyContextMenu(builder, pluginMenuCtx);

        // Sort By (view-only ordering override for the hierarchy tree).
        {
            auto checkedIf = [](bool on) -> uint32_t {
                return on ? static_cast<uint32_t>(MenuItemFlag_Checked) : static_cast<uint32_t>(MenuItemFlag_None);
            };
            // Sort mode and direction are each exactly-one-of; the options
            // below the submenu are independent switches.
            auto oneOf = [&checkedIf](bool on) -> uint32_t {
                return checkedIf(on) | static_cast<uint32_t>(MenuItemFlag_Radio);
            };
            const bool isCustom = (m_SortMode == HierarchySortMode::Custom);
            const bool isAlpha  = (m_SortMode == HierarchySortMode::Alphabetical);
            const bool isType   = (m_SortMode == HierarchySortMode::Type);
            const bool isAsc    = (m_SortDirection == HierarchySortDirection::Ascending);
            const bool isDesc   = (m_SortDirection == HierarchySortDirection::Descending);

            builder.AddItem("Sort By",              0,                                         MenuItemFlag_None,   50, EditorIcons::kSortList);
            builder.AddItem("Sort By/Custom",       HierarchyCommandIds::kCmdSortCustom,       oneOf(isCustom), 50, EditorIcons::kPointer);
            builder.AddItem("Sort By/Alphabetical", HierarchyCommandIds::kCmdSortAlphabetical, oneOf(isAlpha),  51, EditorIcons::kSortList);
            builder.AddItem("Sort By/Type",         HierarchyCommandIds::kCmdSortType,         oneOf(isType),   52, EditorIcons::kTag);
            builder.AddItem("Sort By/-",            0,                                         MenuItemFlag_None,   55);
            builder.AddItem("Sort By/Ascending",    HierarchyCommandIds::kCmdSortAscending,    oneOf(isAsc),    60, EditorIcons::kArrowUp);
            builder.AddItem("Sort By/Descending",   HierarchyCommandIds::kCmdSortDescending,   oneOf(isDesc),   61, EditorIcons::kArrowDown);
            builder.AddItem("Hierarchy Options", 0, MenuItemFlag_None, 70, EditorIcons::kSettings);
            m_LayerControls.BuildMenu(builder);
            // One preference behind both panels, so the label says so rather
            // than implying the Hierarchy has its own.
            if (m_VcsController && m_VcsController->CanToggleIndicatorVisibility())
                builder.AddItem("Hierarchy Options/Show Version Control Dots",
                                HierarchyCommandIds::kCmdShowVcsSceneDiffDots,
                                checkedIf(Editor::AreVcsSceneDiffIndicatorsVisible()),
                                71,
                                EditorIcons::kEye);
        }

        // Scene actions
        builder.AddItem("New Scene", HierarchyCommandIds::kCmdNewScene, MenuItemFlag_None, 100, EditorIcons::kScene);
        m_ContextMenuRecentScenes.clear();
        if (m_GetRecentScenes)
        {
            m_ContextMenuRecentScenes = m_GetRecentScenes();
            if (m_ContextMenuRecentScenes.size() > HierarchyCommandIds::kCmdLoadRecentSceneCount)
                m_ContextMenuRecentScenes.resize(HierarchyCommandIds::kCmdLoadRecentSceneCount);
        }

        builder.AddItem("Load Recent Scene", 0, MenuItemFlag_None, 101, EditorIcons::kScene);
        if (m_ContextMenuRecentScenes.empty())
        {
            builder.AddItem("Load Recent Scene/No Recent Scenes",
                            HierarchyCommandIds::kCmdLoadRecentSceneEmpty,
                            MenuItemFlag_Disabled,
                            101);
        }
        else
        {
            std::unordered_map<std::string, size_t> fileNameCounts;
            for (const auto& scenePath : m_ContextMenuRecentScenes)
                ++fileNameCounts[scenePath.filename().string()];

            for (size_t i = 0; i < m_ContextMenuRecentScenes.size(); ++i)
            {
                const uint32_t cmd = HierarchyCommandIds::kCmdLoadRecentSceneBase + static_cast<uint32_t>(i);
                builder.AddItem("Load Recent Scene/" + BuildRecentSceneMenuLabel(m_ContextMenuRecentScenes[i], fileNameCounts),
                                cmd,
                                MenuItemFlag_None,
                                101 + static_cast<int>(i),
                                EditorIcons::kScene);
            }
        }

        // Script-contributed items
        for (const auto& si : scriptItems)
        {
            if (si.commandId == 0 || si.path.empty())
                continue;
            builder.AddItem(si.path, si.commandId, MenuItemFlag_None, si.priority);
        }
        m_ContextMenu->Clear();
        if (m_ContextMenu->AddSearchField(0, "Search", "", {}))
            m_ContextMenu->AddSeparator(0);
        builder.Build(m_ContextMenu.get());
        m_ContextMenu->Show(m_Window, (int)x, (int)y); };
    m_Tree->SetOnContextMenu(m_ShowContextMenu);

    // Cmd/Ctrl+D: duplicate selected entities.
    m_Tree->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
                                 {
                                     if (!Editor::MatchesCatalogShortcut("Hierarchy", "Duplicate", e.Key, e.Mods))
                                         return;
                                     DuplicateSelectedEntities();
                                     e.Stop(); });

    // Delete key: delete selected entity (and its subtree) as a single undo step.
    m_Tree->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
                                 {
                                     if (!Editor::MatchesCatalogShortcut("Hierarchy", "Delete", e.Key, e.Mods))
                                         return;
                                     if (!m_World || !m_Selection)
                                         return;
                                     const auto selectedIds = m_Selection->GetSelection();
                                     if (selectedIds.empty())
                                         return;

                                     // Collect union of selected subtrees (avoid duplicates).
                                     std::unordered_set<std::uint64_t> seen;
                                     seen.reserve(selectedIds.size() * 8);
                                     std::vector<ECS::EntityHandle> ents;
                                     ents.reserve(selectedIds.size() * 8);

                                     for (auto sid : selectedIds)
                                     {
                                         const TreeId tid = static_cast<TreeId>(sid);
                                         if (tid == 0 || tid == kRootId)
                                             continue;
                                         ECS::EntityHandle h = HierarchyDataProvider::Decode(tid);
                                         if (!h.IsValid() || !m_World->IsValid(h))
                                             continue;
                                         auto subtree = Editor::DeleteEntitiesCommand::CollectSubtree(*m_World, h);
                                         for (auto& se : subtree)
                                         {
                                             if (!se.IsValid() || !m_World->IsValid(se))
                                                 continue;
                                             if (seen.insert((std::uint64_t)se.id).second)
                                                 ents.push_back(se);
                                         }
                                     }

                                     if (ents.empty())
                                         return;

                                     std::sort(ents.begin(), ents.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });

                                     const std::string undoName = (ents.size() > 1) ? "Delete Entities" : "Delete Entity";
                                     if (m_Undo)
                                     {
                                         // Delete + deselect + (on undo) re-select as ONE undo step. The wrapper
                                         // reselects the deleted entities on Undo, so a single Ctrl+Z brings them
                                         // back AND restores selection (redo re-deletes + deselects). The
                                         // applySelection callback (ApplySelectionProgrammatic) suppresses its own
                                         // selection-undo and drives the inspector/gizmo; the deferred Refresh() is
                                         // likewise guarded — so no stray HierarchySelectionCommand lands on top.
                                         std::vector<UI::Interaction::ItemId> selectionBefore =
                                             m_Selection ? m_Selection->GetSelection() : std::vector<UI::Interaction::ItemId>{};
                                         UI::Interaction::ItemId anchorBefore = m_Selection ? m_Selection->GetAnchor() : 0;
                                         auto applySelection = [this](const std::vector<UI::Interaction::ItemId>& ids,
                                                                      UI::Interaction::ItemId anchor)
                                         {
                                             ApplySelectionProgrammatic(ids, anchor);
                                             if (m_OnSelectEntity)
                                             {
                                                 ScopedFlag suppress(m_SuppressSelectionUndo);
                                                 if (ids.empty())
                                                     m_OnSelectEntity({});
                                                 else
                                                     m_OnSelectEntity(HierarchyDataProvider::Decode(static_cast<TreeId>(anchor)));
                                             }
                                         };
                                         auto onDeleted = [applySelection]() { applySelection({}, 0); };
                                         auto onRevived = [applySelection, selectionBefore, anchorBefore]()
                                         { applySelection(selectionBefore, anchorBefore); };
                                         m_Undo->Execute(std::make_unique<Editor::DeleteEntitiesSelectionCommand>(
                                             undoName, m_World, m_ChangeNotifications, std::move(ents),
                                             std::move(onDeleted), std::move(onRevived)));
                                     }
                                     else
                                     {
                                         Editor::DeleteEntitiesCommand cmdObj(
                                             undoName, m_World, m_ChangeNotifications, std::move(ents));
                                         cmdObj.Redo();
                                         if (m_Selection)
                                             m_Selection->Clear();
                                         if (m_OnSelectEntity)
                                             m_OnSelectEntity({});
                                     }
                                     e.Stop(); });

    if (m_Tree)
    {
        m_Tree->SetOnHorizontalBarVisibilityChanged(
            [this](bool visible) { SetHorizontalBarPresent(visible); });
        // Long entity names in a narrow panel must stay reachable, so the tree
        // measures its rows and scrolls sideways. The refresh re-runs the row
        // layout; without it nothing re-measures until an unrelated relayout.
        m_Tree->SetHorizontalScrollEnabled(true);
        m_Tree->RefreshFromProvider();
    }

    AddChild(std::move(tree));

    auto navigationBar = std::make_unique<Editor::HierarchyNavigationBar>();
    m_NavigationBar = navigationBar.get();
    m_NavigationBar->SetItemSize(m_LastTreeIconSizePx);
    m_NavigationBar->SetOnItemSizeChanging([this](float px)
    {
        SetTreeIconSize(px);
        SetTreeRowHeight(DeriveEditorTreeRowHeightFromIconSize(m_LastTreeIconSizePx));
    });
    m_NavigationBar->SetOnItemSizeChanged([this](float px)
    {
        if (m_OnTreeIconSizeWheelCommit)
            m_OnTreeIconSizeWheelCommit(px);
    });

    // Search bar placement and visibility are controlled via Settings.
    auto built = BuildPanelSearchBar(
        "hierarchy-search-field",
        []()
        { return SettingsPanel::GetSearchBarsVisible(); },
        {},
        [this](const std::string& text)
        { ApplySearchFilter(text); },
        {{"all", "All fields"}, {"name", "Name"}, {"type", "Component type"},
         {"path", "Hierarchy path"}, {"id", "Entity ID"}},
        [this](const std::string& scope)
        {
            m_SearchFieldScope = scope;
            ApplySearchFilter(m_SearchField ? m_SearchField->GetValue() : std::string{});
        });
    m_SearchBar = built.RootPtr;
    m_SearchField = built.FieldPtr;
    if (m_SearchField)
        m_SearchField->SetTooltip("Search names, or combine filters: t:Light path:\"/World/Root\" name:Key id:42");
    navigationBar->SetSearchBar(std::move(built.Root));
    AddChild(std::move(navigationBar));
    // The row, not the search inside it, is what the top/bottom setting moves.
    EditorSearchBars::RegisterHosted(m_SearchBar, m_NavigationBar);
}

bool HierarchyPanel::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::OnlineAssetDragPayload>();
}

bool HierarchyPanel::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnEmptySpace;
    out.IndentDepth = 0;
    return true;
}

UI::Interaction::DropFeedback HierarchyPanel::CanDrop(const UI::Interaction::DropRequest& request) const
{
    if (request.payload.Is<Editor::BookmarkDragPayload>())
    {
        if (m_OnNavigateToBookmark)
            return {true, {}};
        return {false, "No navigate callback"};
    }

    if (!m_World)
        return {false, "No world"};

    if (const auto* assetPayload = request.payload.TryGet<Editor::AssetPathsDragPayload>())
    {
        if (!m_Context)
            return {false, "No editor context"};
        if (assetPayload->paths.empty())
            return {false, "Empty payload"};
        return {true, {}};
    }

    if (request.payload.Is<Editor::OnlineAssetDragPayload>())
        return {true, {}};

    if (const auto* entityPayload = request.payload.TryGet<Editor::HierarchyEntityDragPayload>())
        return CanDropEntities(request.hit, *entityPayload);

    return {false, "Wrong payload"};
}

UI::Interaction::DropFeedback HierarchyPanel::CanDropEntities(const UI::Interaction::DropHit& hit,
                                                              const Editor::HierarchyEntityDragPayload& payload) const
{
    if (payload.treeIds.empty())
        return {false, "Empty payload"};

    const ECS::EntityHandle destParent = ResolveDropPlacement(hit).Parent;
    if (!Editor::CanAcceptAuthoredChildren(*m_World, destParent))
        return {false, "Generated - drop onto its parent instead"};

    // Cycle check: cannot parent under any of the moved entities or their descendants.
    for (std::uint64_t tid : payload.treeIds)
    {
        if (tid == 0 || tid == kRootId)
            continue;
        ECS::EntityHandle moved = HierarchyDataProvider::Decode(static_cast<TreeId>(tid));
        if (!moved.IsValid() || !m_World->IsValid(moved))
            continue;

        if (destParent.IsValid() && destParent.id == moved.id)
            return {false, "Cannot parent under self"};
        if (destParent.IsValid() && IsDescendantOf(*m_World, destParent, moved))
            return {false, "Cycle"};
    }

    return {true, {}};
}

void HierarchyPanel::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (const auto* bookmarkPayload = request.payload.TryGet<Editor::BookmarkDragPayload>())
        DropBookmark(request, bookmarkPayload->bookmark);
    else if (const auto* assetPayload = request.payload.TryGet<Editor::AssetPathsDragPayload>())
        DropAssetPaths(request, assetPayload->paths);
    else if (const auto* onlinePayload = request.payload.TryGet<Editor::OnlineAssetDragPayload>())
        DropOnlineAssets(request, *onlinePayload);
    else if (const auto* entityPayload = request.payload.TryGet<Editor::HierarchyEntityDragPayload>())
        DropEntities(request, *entityPayload);
}

void HierarchyPanel::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    (void)state;
}

HierarchyPanel::DropPlacement HierarchyPanel::ResolveDropPlacement(const UI::Interaction::DropHit& hit) const
{
    DropPlacement placement{};
    if (hit.TargetId != 0 && hit.Location != UI::Interaction::DropLocation::OnEmptySpace)
    {
        const ECS::EntityHandle target = HierarchyDataProvider::Decode(static_cast<TreeId>(hit.TargetId));
        if (target.IsValid() && m_World->IsValid(target))
            placement.Target = target;
    }

    if (hit.Location == UI::Interaction::DropLocation::OnItem)
        placement.Parent = placement.Target;
    else if (hit.Location == UI::Interaction::DropLocation::BeforeItem ||
             hit.Location == UI::Interaction::DropLocation::AfterItem)
        placement.Parent = GetEffectiveParent(*m_World, placement.Target); // {} = root
    return placement;
}

void HierarchyPanel::MarkSceneStructureChanged()
{
    NotifyWorldStructure(m_ChangeNotifications, m_World);
    if (m_Context && m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();
}

std::function<void()> HierarchyPanel::SceneDirtyCallback() const
{
    return [ctx = m_Context]()
    {
        if (ctx && ctx->OnSceneDirty)
            ctx->OnSceneDirty();
    };
}

void HierarchyPanel::RebuildTreeAndSelect(ECS::EntityHandle entity)
{
    if (m_Provider)
        m_Provider->Rebuild();
    m_Tree->RefreshFromProvider();
    if (entity.IsValid())
        SelectEntity(entity);
}

void HierarchyPanel::RunDropCommand(std::unique_ptr<Editor::IEditorCommand> command)
{
    if (m_Undo)
        m_Undo->Execute(std::move(command));
    else
        command->Do();
}

void HierarchyPanel::DropBookmark(const UI::Interaction::DropRequest& request, const Bookmark& bookmark)
{
    // An asset bookmark drops as the asset it names: a Polyhaven reference as
    // the online asset, a GUID as its file. Any other bookmark navigates.
    if (bookmark.Type == BookmarkType::Asset && m_World && m_Context)
    {
        if (bookmark.Reference.starts_with(kPolyhavenBookmarkPrefix))
        {
            const std::string slug = bookmark.Reference.substr(kPolyhavenBookmarkPrefix.size());
            Editor::OnlineAssetDragPayload onlinePayload;
            onlinePayload.slug = slug;
            onlinePayload.name = bookmark.Name;
            onlinePayload.type = "models";
            onlinePayload.entries.push_back({slug, bookmark.Name});
            DropOnlineAssets(request, onlinePayload);
            return;
        }

        const GUID guid(bookmark.Reference);
        AssetMetadata metadata;
        if (!guid.IsNull() &&
            EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, metadata) &&
            !metadata.Path.empty())
        {
            std::filesystem::path absPath = metadata.Path;
            if (!absPath.is_absolute() && !m_Context->AssetsRoot.empty())
                absPath = m_Context->AssetsRoot / absPath;
            DropAssetPaths(request, {std::move(absPath)});
            return;
        }
    }

    if (m_OnNavigateToBookmark)
        m_OnNavigateToBookmark(bookmark);
}

void HierarchyPanel::DropAssetPaths(const UI::Interaction::DropRequest& request,
                                    const std::vector<std::filesystem::path>& paths)
{
    if (!m_World || !m_Context)
        return;

    // Asset drops parent under a row only when dropped onto it; between rows they land at the root.
    const ECS::EntityHandle parent = request.hit.Location == UI::Interaction::DropLocation::OnItem
                                         ? ResolveDropPlacement(request.hit).Parent
                                         : ECS::EntityHandle{};

    if (paths.size() == 1 && !paths[0].empty() && m_Context->Assets && DropSingleAsset(paths[0], parent))
        return;

    // Parenting keeps the model's own scale instead of multiplying it by the parent's.
    Mathematics::Vector3 parentInvScale{1.0f, 1.0f, 1.0f};
    if (parent.IsValid())
    {
        if (const auto* wt = m_World->GetComponent<Components::WorldTransform>(parent))
        {
            Components::Transform tmp;
            std::memcpy(tmp.matrix, wt->matrix, sizeof(tmp.matrix));
            auto ps = tmp.GetScale();
            if (ps.x > 0.0f) parentInvScale.x = 1.0f / ps.x;
            if (ps.y > 0.0f) parentInvScale.y = 1.0f / ps.y;
            if (ps.z > 0.0f) parentInvScale.z = 1.0f / ps.z;
        }
    }

    std::vector<ECS::EntityHandle> createdEntities;
    for (const auto& absPath : paths)
    {
        if (absPath.empty())
            continue;

        const std::string ext = absPath.extension().string();
        if (GetAssetTypeFromExtension(ext) == AssetType::LensFlareDefinition && m_Context->Assets)
        {
            const GUID flareGuid = m_Context->Assets->ResolveAssetGuid(absPath);
            if (!flareGuid.IsNull())
            {
                const ECS::EntityHandle flareEntity = CreateLensFlareEntity(*m_World, absPath, flareGuid, parent);
                if (flareEntity.IsValid())
                    createdEntities.push_back(flareEntity);
            }
            continue;
        }

        if (IsModelFileExtension(ext) && m_Context->Assets && m_Context->RenderServices)
        {
            const GUID modelGuid = m_Context->Assets->ResolveAssetGuid(absPath);
            if (!modelGuid.IsNull())
            {
                SpawnModelWhenLoaded(absPath, modelGuid, parent, parentInvScale);
                continue;
            }
        }

        const ECS::EntityHandle blueprintEntity = CreateBlueprintInstanceEntity(*m_World, absPath, parent);
        if (blueprintEntity.IsValid())
            createdEntities.push_back(blueprintEntity);
    }

    if (!createdEntities.empty())
        Editor::AppendUnorderedRootsToHierarchyEnd(*m_World, createdEntities);
    MarkSceneStructureChanged();
    RebuildTreeAndSelect(createdEntities.empty() ? ECS::EntityHandle{} : createdEntities.back());
}

bool HierarchyPanel::DropSingleAsset(const std::filesystem::path& path, ECS::EntityHandle parent)
{
    AssetManager& assets = *m_Context->Assets;
    if (Editor::IsHdriPath(path))
    {
        const ECS::EntityHandle skybox =
            Editor::CreateSkyboxEntityFromHdri(*m_World, assets, path, m_ChangeNotifications, SceneDirtyCallback());
        if (skybox.IsValid())
            RebuildTreeAndSelect(skybox);
        return true;
    }

    const AssetType assetType = GetAssetTypeFromExtension(path.extension().string());
    const bool onMesh = parent.IsValid() && m_World->GetComponent<Components::MeshRenderer>(parent) != nullptr;

    if (assetType == AssetType::Texture && onMesh)
    {
        const GUID textureGuid = assets.ResolveAssetGuid(path);
        if (textureGuid.IsNull())
            return false;
        RunDropCommand(std::make_unique<Editor::TextureDropOnMeshCommand>(
            "Assign Texture", m_World, parent, textureGuid, "albedoMap", std::string(),
            SceneDirtyCallback(), m_ChangeNotifications));
        return true;
    }

    // A texture that lands off a mesh becomes a plane sprite with the texture's aspect ratio.
    if (assetType == AssetType::Texture && m_Context->RenderServices)
    {
        const bool is2D = m_Context->IsSceneView2D && m_Context->IsSceneView2D();
        const ECS::EntityHandle sprite = Editor::CreateSpriteEntityFromTexture(
            *m_World, *m_Context->RenderServices, assets, path, Mathematics::Vector3{0.0f, 0.0f, 0.0f}, is2D);
        if (!sprite.IsValid())
            return false;
        ParentUnder(*m_World, sprite, parent);
        Editor::AppendUnorderedRootsToHierarchyEnd(*m_World, std::span(&sprite, 1));
        MarkSceneStructureChanged();
        RebuildTreeAndSelect(sprite);
        return true;
    }

    if (assetType == AssetType::Material && onMesh)
    {
        const GUID materialGuid = assets.ResolveAssetGuid(path);
        if (materialGuid.IsNull())
            return false;
        if (!assets.GetAsset(materialGuid))
            assets.LoadAsset(materialGuid, AssetLoadResultCallback{});
        RunDropCommand(std::make_unique<Editor::MaterialDropOnMeshCommand>(
            "Assign Material", m_World, parent, materialGuid, SceneDirtyCallback(), m_ChangeNotifications));
        return true;
    }

    return false;
}

void HierarchyPanel::SpawnModelWhenLoaded(const std::filesystem::path& modelPath,
                                          const GUID& modelGuid,
                                          ECS::EntityHandle parent,
                                          const Mathematics::Vector3& parentInvScale)
{
    // The load runs asynchronously and the model spawns on the UI-thread
    // continuation, unselected. The context and world outlive the load.
    const EditorContext* ctx = m_Context;
    ECS::World* world = m_World;
    Editor::RunWhenAssetLoaded(*m_Context->Assets, modelGuid, AssetLoadPriority::High, this, world,
        [ctx, world, modelGuid, modelPath, parent, parentInvScale]()
        {
            if (!ctx || !world || !ctx->Assets || !ctx->RenderServices)
                return;
            AssetManager& assets = *ctx->Assets;
            SharedPtr<Asset> asset = assets.GetAsset(modelGuid);
            auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
            if (!modelAsset || !modelAsset->IsLoaded())
                return;

            auto result = Engine::Renderer::ModelEntityFactory::CreateFromModel(
                *ctx->RenderServices, *world, *modelAsset, modelGuid, modelPath.stem().string(),
                Editor::GetFbxModelEntityFactoryOptions(modelPath));
            if (!result.IsValid())
                return;

            Editor::ExportModelMaterials(modelPath, modelGuid, result.submeshEntities, *world, assets);
            Editor::AppendUnorderedRootsToHierarchyEnd(*world, std::span(&result.rootEntity, 1));
            if (parent.IsValid())
            {
                ParentUnder(*world, result.rootEntity, parent);
                if (auto* lt = world->GetComponentForWrite<Components::Transform>(result.rootEntity))
                {
                    auto childScale = lt->GetScale();
                    lt->SetScale(childScale.x * parentInvScale.x,
                                 childScale.y * parentInvScale.y,
                                 childScale.z * parentInvScale.z);
                }
            }

            if (ctx->OnSceneDirty)
                ctx->OnSceneDirty();
        }, modelPath.filename().string());
}

void HierarchyPanel::DropOnlineAssets(const UI::Interaction::DropRequest& request,
                                      const Editor::OnlineAssetDragPayload& payload)
{
    if (!m_World || !m_Context || payload.slug.empty())
        return;

    std::vector<Editor::OnlineAssetEntry> items = payload.entries;
    if (items.empty())
        items.push_back({payload.slug, payload.name});

    if (payload.type == "hdris")
    {
        ECS::EntityHandle lastSkybox{};
        for (const auto& item : items)
        {
            const ECS::EntityHandle skybox =
                Editor::CreateSkyboxEntityFromPolyhavenHdri(*m_World, *m_Context, item.slug, item.name,
                                                            m_ChangeNotifications);
            if (skybox.IsValid())
                lastSkybox = skybox;
        }

        if (lastSkybox.IsValid())
            RebuildTreeAndSelect(lastSkybox);
        return;
    }

    const DropPlacement placement = ResolveDropPlacement(request.hit);

    // Reparenting keeps the spawned entity's world pose.
    glm::mat4 parentWorldInv(1.0f);
    if (placement.Parent.IsValid())
    {
        if (const auto* wt = m_World->GetComponent<Components::WorldTransform>(placement.Parent))
            parentWorldInv = glm::inverse(glm::make_mat4(wt->matrix));
    }

    std::vector<ECS::EntityHandle> createdEntities;
    for (const auto& item : items)
    {
        ECS::EntityHandle createdEntity = SpawnDownloadedOnlineAsset(item);
        if (createdEntity.IsValid() && placement.Parent.IsValid())
        {
            ParentUnder(*m_World, createdEntity, placement.Parent);
            if (auto* lt = m_World->GetComponentForWrite<Components::Transform>(createdEntity))
            {
                glm::mat4 childLocal = glm::make_mat4(lt->matrix);
                glm::mat4 newLocal = parentWorldInv * childLocal;
                std::memcpy(lt->matrix, &newLocal[0][0], sizeof(float32) * 16);
            }
        }
        if (!createdEntity.IsValid())
            createdEntity = CreateOnlineAssetPlaceholder(item, payload.type, placement.Parent);
        if (createdEntity.IsValid())
            createdEntities.push_back(createdEntity);
    }

    if (!createdEntities.empty())
    {
        WriteSiblingOrder(*m_World,
                          SiblingsWithDropInsertion(*m_World, placement.Parent, placement.Target,
                                                    request.hit.Location, createdEntities));
    }

    MarkSceneStructureChanged();
    RebuildTreeAndSelect(createdEntities.empty() ? ECS::EntityHandle{} : createdEntities.back());
}

ECS::EntityHandle HierarchyPanel::SpawnDownloadedOnlineAsset(const Editor::OnlineAssetEntry& item)
{
    // Project assets first, then an early download completed into the temp cache.
    const std::filesystem::path assetsRoot = m_Context->AssetsRoot;
    std::filesystem::path mainFilePath = PolyhavenService::FindDownloadedFile(item.slug, assetsRoot);
    if (mainFilePath.empty())
    {
        std::filesystem::path cachedFile = PolyhavenService::FindCachedDownloadFile(item.slug);
        if (!cachedFile.empty())
        {
            std::filesystem::path projectDir = assetsRoot / "Polyhaven" / item.slug;
            mainFilePath = PolyhavenService::MoveDownloadToProject(cachedFile, cachedFile.parent_path(), projectDir);
        }
    }
    if (mainFilePath.empty() || !m_Context->Assets || !m_Context->RenderServices)
        return {};

    AssetManager& assets = *m_Context->Assets;
    const std::string ext = mainFilePath.extension().string();
    if (GetAssetTypeFromExtension(ext) == AssetType::Texture)
    {
        const bool is2D = m_Context->IsSceneView2D && m_Context->IsSceneView2D();
        const ECS::EntityHandle sprite = Editor::CreateSpriteEntityFromTexture(
            *m_World, *m_Context->RenderServices, assets, mainFilePath, Mathematics::Vector3{}, is2D);
        if (sprite.IsValid())
            Logger::Log::Info("HierarchyPanel: dropped texture '{}' as plane sprite", item.slug);
        else
            Logger::Log::Warning("HierarchyPanel: CreateSpriteEntityFromTexture failed for '{}'", item.slug);
        return sprite;
    }

    if (!IsModelFileExtension(ext))
        return {};

    const GUID assetGuid = assets.ResolveAssetGuid(mainFilePath);
    if (assetGuid.IsNull())
    {
        Logger::Log::Warning("HierarchyPanel: ResolveAssetGuid null for '{}' path={}", item.slug, mainFilePath.string());
        return {};
    }

    SharedPtr<Asset> asset = assets.GetAsset(assetGuid);
    if (!asset)
        asset = assets.LoadAssetAsync(assetGuid, AssetLoadPriority::High).get();
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return {};

    auto result = Engine::Renderer::ModelEntityFactory::CreateFromModel(
        *m_Context->RenderServices, *m_World, *modelAsset, assetGuid, item.name,
        Editor::GetFbxModelEntityFactoryOptions(mainFilePath));
    if (!result.IsValid())
    {
        Logger::Log::Warning("HierarchyPanel: CreateFromModel failed for '{}'", item.slug);
        return {};
    }

    Editor::ExportModelMaterials(mainFilePath, assetGuid, result.submeshEntities, *m_World, assets);
    Editor::AppendUnorderedRootsToHierarchyEnd(*m_World, std::span(&result.rootEntity, 1));
    Logger::Log::Info("HierarchyPanel: dropped model '{}'", item.slug);
    return result.rootEntity;
}

ECS::EntityHandle HierarchyPanel::CreateOnlineAssetPlaceholder(const Editor::OnlineAssetEntry& item,
                                                               const std::string& assetType,
                                                               ECS::EntityHandle parent)
{
    if (!m_Context->RenderServices)
        return {};

    Logger::Log::Info("HierarchyPanel: creating placeholder for '{}'", item.slug);
    const Mathematics::Vector3 dropPos{};
    const Mathematics::Vector3 defaultCamPos{0.0f, 0.0f, -5.0f};
    const std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (item.slug + ".png");
    const auto placeholderResult = PolyhavenPlaceholderFactory::Create(
        *m_World, *m_Context->RenderServices, m_Context->Assets,
        item.slug, assetType, dropPos, defaultCamPos, thumbPath, parent);
    const ECS::EntityHandle placeholder = placeholderResult.entity;
    if (!placeholder.IsValid())
        return {};

    // Hide the placeholder billboard; the scene download pill is the indicator.
    if (m_World->HasComponent<Components::MeshRenderer>(placeholder))
        ECS::Entity(m_World, placeholder).SetEnabled<Components::MeshRenderer>(false);
    if (m_Context->DownloadManager)
    {
        if (m_Context->DownloadManager->IsDownloadingOrCompleted(item.slug))
            m_Context->DownloadManager->AssignPlaceholder(item.slug, placeholder, m_World);
        else
            m_Context->DownloadManager->StartDownloadForPlaceholder(
                item.slug, assetType, m_Context->AssetsRoot, placeholder, m_World);
    }
    return placeholder;
}

void HierarchyPanel::DropEntities(const UI::Interaction::DropRequest& req,
                                  const Editor::HierarchyEntityDragPayload& payload)
{
    if (!m_World || payload.treeIds.empty())
        return;

    const DropPlacement placement = ResolveDropPlacement(req.hit);
    const ECS::EntityHandle destParent = placement.Parent;

    // Build moved entity list.
    std::vector<ECS::EntityHandle> moved;
    moved.reserve(payload.treeIds.size());
    for (std::uint64_t tid : payload.treeIds)
    {
        if (tid == 0 || tid == kRootId)
            continue;
        ECS::EntityHandle h = HierarchyDataProvider::Decode(static_cast<TreeId>(tid));
        if (h.IsValid() && m_World->IsValid(h))
            moved.push_back(h);
    }
    if (moved.empty())
        return;

    // Cycle guard (commit-time, too).
    for (auto& m : moved)
    {
        if (destParent.IsValid() && destParent.id == m.id)
            return;
        if (destParent.IsValid() && IsDescendantOf(*m_World, destParent, m))
            return;
    }

    // Moved entities keep their relative order.
    SortByOrderThenId(*m_World, moved);
    const std::vector<ECS::EntityHandle> dstList =
        SiblingsWithDropInsertion(*m_World, destParent, placement.Target, req.hit.Location, moved);

    // Determine which entities are affected: moved + any entities in the destination list (order renormalized).
    std::vector<ECS::EntityHandle> affected = dstList;
    for (auto& m : moved)
        affected.push_back(m);
    std::sort(affected.begin(), affected.end(), [](auto& a, auto& b) { return a.id < b.id; });
    affected.erase(std::unique(affected.begin(), affected.end(), [](auto& a, auto& b) { return a.id == b.id; }), affected.end());

    std::vector<Editor::ReparentReorderEntitiesCommand::EntityState> before;
    before.reserve(affected.size());
    for (auto& e : affected)
        before.push_back(CaptureReparentState(*m_World, e));

    // Apply parent changes for moved immediately (we commit already-applied for undo).
    // Adjust local transforms so world-space position/rotation/scale are preserved.
    for (auto& m : moved)
    {
        if (destParent.IsValid())
        {
            Components::Parent p{};
            p.parent = destParent;
            m_World->AddComponentImmediate(m, p);

            auto* childWorld = m_World->GetComponent<Components::WorldTransform>(m);
            auto* parentWorld = m_World->GetComponent<Components::WorldTransform>(destParent);
            if (childWorld && parentWorld)
            {
                glm::mat4 parentMat = glm::make_mat4(parentWorld->matrix);
                glm::mat4 childMat = glm::make_mat4(childWorld->matrix);
                glm::mat4 newLocal = glm::inverse(parentMat) * childMat;

                Transform xf{};
                std::memcpy(xf.matrix, &newLocal[0][0], sizeof(float32) * 16);
                m_World->AddComponentImmediate(m, xf);
            }
        }
        else
        {
            // Moving to root: local transform becomes the current world transform.
            auto* childWorld = m_World->GetComponent<Components::WorldTransform>(m);
            if (childWorld)
            {
                Transform xf{};
                std::memcpy(xf.matrix, childWorld->matrix, sizeof(float32) * 16);
                m_World->AddComponentImmediate(m, xf);
            }
            m_World->RemoveComponentImmediate<Components::Parent>(m);
        }
    }

    WriteSiblingOrder(*m_World, dstList);

    std::vector<Editor::ReparentReorderEntitiesCommand::EntityState> after;
    after.reserve(affected.size());
    for (auto& e : affected)
        after.push_back(CaptureReparentState(*m_World, e));

    // Group the reparent and the follow-up selection change into one undo step so a single
    // Ctrl+Z restores both the hierarchy AND the selection that was active before the drag.
    // (m_LastSelectionIds still holds the pre-drag selection — the deferred-on-mouse-up tree
    // never committed a selection during the drag.)
    const std::vector<UI::Interaction::ItemId> beforeSel = m_LastSelectionIds;
    const UI::Interaction::ItemId beforeAnchor = m_LastSelectionAnchor;

    // RAII scope-exit: guarantee EndCompound runs (even if an op below throws), otherwise an
    // unbalanced compound stack would corrupt all subsequent undo/redo.
    std::shared_ptr<void> compoundScope;
    if (m_Undo)
    {
        m_Undo->BeginCompound("Reorder Entities");
        compoundScope = std::shared_ptr<void>(static_cast<void*>(nullptr),
                                              [undo = m_Undo](void*) { undo->EndCompound(); });
    }

    if (m_Undo)
    {
        auto cmd = std::make_unique<Editor::ReparentReorderEntitiesCommand>(
            "Reparent Entities",
            m_World,
            m_ChangeNotifications,
            std::move(before),
            std::move(after));
        m_Undo->CommitAlreadyApplied(std::move(cmd));
    }

    NotifyWorldStructure(m_ChangeNotifications, m_World);

    if (m_Provider)
        m_Provider->Rebuild();
    m_Tree->RefreshFromProvider();

    // A reorder/reparent selects the moved entities — the tree defers click-selection to
    // mouse-up and a drag never commits it, so without this the drag would leave the prior
    // selection in place. Drops onto an inspector EntityField are consumed by the field and
    // never reach this handler, so that path keeps the current selection (and inspector).
    // Drive the selection through the normal changed-callback (not the undo-suppressing
    // ApplySelectionProgrammatic) so the selection change is recorded inside the compound and
    // the inspector follows the moved entity.
    std::vector<UI::Interaction::ItemId> movedIds;
    movedIds.reserve(moved.size());
    for (ECS::EntityHandle h : moved)
    {
        const TreeId tid = HierarchyDataProvider::Encode(h);
        if (tid != 0 && tid != kRootId)
            movedIds.push_back(static_cast<UI::Interaction::ItemId>(tid));
    }
    if (m_Selection && !movedIds.empty())
    {
        m_LastSelectionIds = beforeSel;
        m_LastSelectionAnchor = beforeAnchor;
        m_Selection->SetSelection(movedIds, movedIds[0]);
        m_Tree->SyncSelectionVisuals();
    }
    // compoundScope's dtor calls EndCompound here, when the handler scope exits.
}

void HierarchyPanel::DuplicateSelectedEntities()
{
    if (!m_World || !m_Selection)
        return;

    // Gather selected root entities (skip those whose parent is also selected).
    const auto selectedIds = m_Selection->GetSelection();
    if (selectedIds.empty())
        return;

    std::vector<ECS::EntityHandle> roots;
    for (auto sid : selectedIds)
    {
        const TreeId tid = static_cast<TreeId>(sid);
        if (tid == 0 || tid == kRootId)
            continue;
        ECS::EntityHandle h = HierarchyDataProvider::Decode(tid);
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;
        roots.push_back(h);
    }
    if (roots.empty())
        return;

    const Editor::EntityDuplicateResult duplicate = Editor::DuplicateEntitySubtreeRoots(*m_World, roots);
    std::vector<ECS::EntityHandle> newRoots = duplicate.NewRoots;

    if (m_Undo && !newRoots.empty())
    {
        // NewRoots only lists the duplicated top roots; the command must own the
        // clones' full subtrees so undo removes every cloned entity.
        std::vector<ECS::EntityHandle> clones;
        clones.reserve(newRoots.size() * 4);
        for (const auto& nr : newRoots)
        {
            auto subtree = Editor::DeleteEntitiesCommand::CollectSubtree(*m_World, nr);
            clones.insert(clones.end(), subtree.begin(), subtree.end());
        }

        // Redo re-selects the clones; Undo restores the pre-duplicate selection —
        // both as part of THIS undo entry, mirroring the delete path. The
        // applySelection callback suppresses its own selection-undo so no stray
        // HierarchySelectionCommand lands on top.
        std::vector<UI::Interaction::ItemId> selectionBefore = m_Selection->GetSelection();
        UI::Interaction::ItemId anchorBefore = m_Selection->GetAnchor();
        std::vector<UI::Interaction::ItemId> cloneIds;
        cloneIds.reserve(newRoots.size());
        for (const auto& nr : newRoots)
            cloneIds.push_back(HierarchyDataProvider::Encode(nr));

        auto applySelection = [this](const std::vector<UI::Interaction::ItemId>& ids,
                                     UI::Interaction::ItemId anchor)
        {
            ApplySelectionProgrammatic(ids, anchor);
            if (m_OnSelectEntity)
            {
                ScopedFlag suppress(m_SuppressSelectionUndo);
                if (ids.empty())
                    m_OnSelectEntity({});
                else
                    m_OnSelectEntity(HierarchyDataProvider::Decode(static_cast<TreeId>(anchor)));
            }
        };
        auto onClonesRestored = [applySelection, cloneIds]()
        { applySelection(cloneIds, cloneIds.front()); };
        auto onClonesRemoved = [applySelection, selectionBefore, anchorBefore]()
        { applySelection(selectionBefore, anchorBefore); };

        const std::string undoName = (newRoots.size() > 1) ? "Duplicate Entities" : "Duplicate Entity";
        m_Undo->CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>(
            undoName, m_World, m_ChangeNotifications, std::move(clones),
            std::move(onClonesRestored), std::move(onClonesRemoved)));
    }

    // Notify world structure changed so the hierarchy rebuilds.
    NotifyWorldStructure(m_ChangeNotifications, m_World);

    if (newRoots.empty())
        return;

    this->PostSafeAction([this, newRoots = std::move(newRoots)]()
                         {
                             if (!m_World || !m_Selection)
                                 return;

                             std::vector<UI::Interaction::ItemId> newIds;
                             std::vector<ECS::EntityHandle> validRoots;
                             newIds.reserve(newRoots.size());
                             validRoots.reserve(newRoots.size());
                             for (const auto& nr : newRoots)
                             {
                                 if (nr.IsValid() && m_World->IsValid(nr))
                                 {
                                     newIds.push_back(HierarchyDataProvider::Encode(nr));
                                     validRoots.push_back(nr);
                                 }
                             }
                             if (newIds.empty())
                                 return;

                             m_SuppressSelectionUndo = true;
                             m_Selection->SetSelection(newIds, newIds.front());
                             m_LastSelectionIds = m_Selection->GetSelection();
                             m_LastSelectionAnchor = m_Selection->GetAnchor();
                             m_SuppressSelectionUndo = false;

                             if (m_Tree)
                                 m_Tree->RefreshFromProvider();

                             if (validRoots.size() > 1)
                             {
                                 if (m_OnSelectEntities)
                                     m_OnSelectEntities(validRoots);
                             }
                             else if (m_OnSelectEntity)
                             {
                                 m_OnSelectEntity(validRoots.front());
                             } });
}

bool HierarchyPanel::HandleCommand(std::uint32_t cmd)
{
    if (IsLoadRecentSceneCommand(cmd))
    {
        const size_t index = static_cast<size_t>(cmd - HierarchyCommandIds::kCmdLoadRecentSceneBase);
        if (index < m_ContextMenuRecentScenes.size() && m_OnOpenRecentScene)
            m_OnOpenRecentScene(m_ContextMenuRecentScenes[index]);
        return true;
    }
    if (cmd == HierarchyCommandIds::kCmdLoadRecentSceneEmpty)
        return true;

    if (!m_World)
        return false;















    if (m_LayerControls.HandleCommand(cmd))
    {
        if (m_Tree)
            m_Tree->RefreshFromProvider();
        return true;
    }
    switch (cmd)
    {
    case HierarchyCommandIds::kCmdCreateEmptyEntity:
        CreateEntityFromMenu("Create Empty Entity", "Empty Entity", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateAsEmptyParent:
        CreateEmptyParentForContextTarget();
        return true;
    case HierarchyCommandIds::kCmdCreateShapeCube:
        CreateEntityFromMenu("Create Cube", "Cube", /*withRenderer*/ true, 1u, /*withCamera*/ false, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::DynamicBox);
        return true;
    case HierarchyCommandIds::kCmdCreateShapeSphere:
        CreateEntityFromMenu("Create Sphere", "Sphere", /*withRenderer*/ true, 2u, /*withCamera*/ false, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::DynamicSphere);
        return true;
    case HierarchyCommandIds::kCmdCreateShapeCapsule:
        CreateEntityFromMenu("Create Capsule", "Capsule", /*withRenderer*/ true, 3u, /*withCamera*/ false, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::DynamicCapsule);
        return true;
    case HierarchyCommandIds::kCmdCreateShapePlane:
        CreateEntityFromMenu("Create Plane", "Plane", /*withRenderer*/ true, 4u, /*withCamera*/ false, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::StaticPlaneBox);
        return true;
    case HierarchyCommandIds::kCmdCreateCamera:
        CreateEntityFromMenu("Create Camera", "Camera", /*withRenderer*/ false, 0u, /*withCamera*/ true, /*withLight*/ false, Components::LightType::Directional, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateLightDirectional:
        CreateEntityFromMenu("Create Directional Light", "Directional Light", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ true, Components::LightType::Directional, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateLightPoint:
        CreateEntityFromMenu("Create Point Light", "Point Light", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ true, Components::LightType::Point, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateLightSpot:
        CreateEntityFromMenu("Create Spot Light", "Spot Light", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ true, Components::LightType::Spot, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateLightArea:
        CreateEntityFromMenu("Create Area Light", "Area Light", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ true, Components::LightType::Area, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateLightAmbient:
        CreateEntityFromMenu("Create Ambient Light", "Ambient Light", /*withRenderer*/ false, 0u, /*withCamera*/ false, /*withLight*/ true, Components::LightType::Ambient, PhysicsSpawn::None);
        return true;
    case HierarchyCommandIds::kCmdCreateOcean:
    CreateOceanEntity();
        return true;
    case HierarchyCommandIds::kCmdCreateSkyEnvironment:
    {
        if (!m_World)
            return true;

        const ECS::EntityHandle h = Editor::CreateSkyEnvironmentEntity(*m_World, m_Undo, m_ChangeNotifications);
        NotifyWorldStructure(m_ChangeNotifications, m_World);
        ScheduleSelectCreatedEntity(h);
        return true;
    }
    case HierarchyCommandIds::kCmdCreateTerrain:
    case HierarchyCommandIds::kCmdCreateTerrainLarge:
    case HierarchyCommandIds::kCmdCreatePlanet5km:
    case HierarchyCommandIds::kCmdCreatePlanet50km:
    CreateTerrainEntity(cmd);
        return true;
    case HierarchyCommandIds::kCmdCreateDDGIVolume:
    CreateDDGIVolumeEntity();
        return true;
    case HierarchyCommandIds::kCmdCreatePostProcessVolume:
    CreatePostProcessVolumeEntity();
        return true;
    case HierarchyCommandIds::kCmdCreateWindVolume:
    CreateWindVolumeEntity();
        return true;
    case HierarchyCommandIds::kCmdCreateReflectionProbe:
        CreateReflectionProbeEntity();
        return true;
    case HierarchyCommandIds::kCmdCreateParticleEmitter:
        CreateParticleEmitterEntity();
        return true;
    case HierarchyCommandIds::kCmdNewScene:
        if (m_OnNewScene)
            m_OnNewScene();
        return true;
    case HierarchyCommandIds::kCmdDuplicate:
        DuplicateSelectedEntities();
        return true;
    case HierarchyCommandIds::kCmdSortCustom:
        m_SortMode = HierarchySortMode::Custom;
        ApplySortSettings();
        return true;
    case HierarchyCommandIds::kCmdSortAlphabetical:
        m_SortMode = HierarchySortMode::Alphabetical;
        ApplySortSettings();
        return true;
    case HierarchyCommandIds::kCmdSortType:
        m_SortMode = HierarchySortMode::Type;
        ApplySortSettings();
        return true;
    case HierarchyCommandIds::kCmdSortAscending:
        m_SortDirection = HierarchySortDirection::Ascending;
        ApplySortSettings();
        return true;
    case HierarchyCommandIds::kCmdSortDescending:
        m_SortDirection = HierarchySortDirection::Descending;
        ApplySortSettings();
        return true;
    case HierarchyCommandIds::kCmdShowVcsSceneDiffDots:
        if (m_VcsController)
            m_VcsController->ToggleIndicatorVisibility();
        return true;
    case HierarchyCommandIds::kCmdShowMeshLocation:
    ShowContextTargetMeshLocation();
        return true;
    case HierarchyCommandIds::kCmdAddToBookmarks:
    AddSelectionToBookmarks();
        return true;
    default:
        break;
    }

    return TryHandlePluginCommand(cmd);
}

void HierarchyPanel::ScheduleSelectCreatedEntity(ECS::EntityHandle entity)
{
    if (!entity.IsValid())
        return;
    this->PostSafeAction([this, entity]()
                         {
                             if (!m_World || !m_World->IsValid(entity))
                                 return;

                             if (m_Selection)
                             {
                                 m_SuppressSelectionUndo = true;
                                 m_Selection->SetSingle(HierarchyDataProvider::Encode(entity));
                                 m_LastSelectionIds = m_Selection->GetSelection();
                                 m_LastSelectionAnchor = m_Selection->GetAnchor();
                                 m_SuppressSelectionUndo = false;
                             }

                             if (m_Tree)
                                 m_Tree->RefreshFromProvider();

                             if (m_OnSelectEntity)
                                 m_OnSelectEntity(entity);

                             if (m_OnEntityCreated)
                                 m_OnEntityCreated(entity); });
}

void HierarchyPanel::CreateEntityFromMenu(const std::string& undoName, const std::string& displayName,
                              bool withRenderer, uint32_t meshId, bool withCamera, bool withLight,
                              Components::LightType lightType, PhysicsSpawn physics)
{
    ECS::World* world = m_World;
    if (!world)
        return;

    const bool hasParent = m_ContextTargetEntity.IsValid() && world->IsValid(m_ContextTargetEntity);
    Parent parentComp{};
    if (hasParent)
        parentComp.parent = m_ContextTargetEntity;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    Transform xf = MakeIdentityTransform();
    if (withLight && lightType == Components::LightType::Directional)
    {
        xf = Transform::FromTRS(
            Mathematics::Vector3{},
            Components::QuaternionFromEulerXYZDegrees(
                Components::kDefaultDirectionalLightEulerXDeg,
                Components::kDefaultDirectionalLightEulerYDeg,
                Components::kDefaultDirectionalLightEulerZDeg),
            Mathematics::Vector3{1.0f, 1.0f, 1.0f});
    }
    Name nm = MakeName(displayName);

    world->AddComponentImmediate(h, xf);
    world->AddComponentImmediate(h, nm);
    if (hasParent)
        world->AddComponentImmediate(h, parentComp);

    MeshRenderer mr{};
    if (withRenderer)
    {
        mr.meshId = meshId;
        mr.renderLayerMask = 1u; // Scene View layer only (for now)

        // Resolve primitive GUID once (single source of truth — adding
        // a 5th primitive ID requires updating only this switch).
        GUID meshGuid;
        switch (meshId)
        {
        case 1u:
            meshGuid = Engine::Renderer::PrimitiveGenerator::CubeGuid();
            break;
        case 2u:
            meshGuid = Engine::Renderer::PrimitiveGenerator::SphereGuid();
            break;
        case 3u:
            meshGuid = Engine::Renderer::PrimitiveGenerator::CapsuleGuid();
            break;
        case 4u:
            meshGuid = Engine::Renderer::PrimitiveGenerator::PlaneGuid();
            break;
        default:
            meshGuid = GUID::Null();
            break;
        }

        if (m_Context && m_Context->RenderServices && !meshGuid.IsNull())
        {
            auto matGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
            mr = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(
                m_Context->RenderServices, meshGuid, matGuid);
        }

        world->AddComponentImmediate(h, mr);

        // Primitives carry their own LocalBounds so spatial queries
        // (picking, culling, AI LOS) see real bounds rather than
        // per-system fallback rules. Matches BuiltInSceneSchemas.
        if (Engine::Renderer::PrimitiveGenerator::IsPrimitive(meshGuid))
        {
            world->AddComponentImmediate(h,
                                         Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(
                                             m_Context ? m_Context->RenderServices : nullptr, meshGuid));
        }
    }

    Components::Camera cam{};
    if (withCamera)
    {
        world->AddComponentImmediate(h, cam);
    }

    Components::Light light{};
    if (withLight)
    {
        light.Type = lightType;
        // Physical, realistic defaults per light type (Area/Ambient/Volume have no physical unit,
        // so they stay Unitless).
        if (light.Type == Components::LightType::Directional)
        {
            light.Intensity = Components::kClearNoonSunIlluminanceLux;
            light.IntensityUnit = Components::LightUnit::Lux;
        }
        else if (light.Type == Components::LightType::Point)
        {
            light.Intensity = 1500.0f;                       // a bright household bulb (~1500 lm)
            light.IntensityUnit = Components::LightUnit::Lumen;
            light.Range = 10.0f;
        }
        else if (light.Type == Components::LightType::Spot)
        {
            light.Intensity = 3000.0f;                       // a bright downlight/spot (~3000 lm)
            light.IntensityUnit = Components::LightUnit::Lumen;
            light.Range = 12.0f;
            light.InnerAngle = 0.35f;
            light.OuterAngle = 0.6f;
        }
        else if (light.Type == Components::LightType::Ambient)
        {
            light.Intensity = 0.020f;
        }
        else if (light.Type == Components::LightType::Area)
        {
            light.Intensity = 40.0f;
            light.Range = 12.0f;
            light.AreaShape = Components::AreaLightShape::Rectangle;
            light.AreaWidth = 2.0f;
            light.AreaHeight = 2.0f;
            light.AreaRadius = 1.0f;
            light.Decay = 2.0f;
        }
        else if (light.Type == Components::LightType::Volume)
        {
            light.Intensity = 8.0f;
            light.Range = 8.0f;
            light.Decay = 2.0f;
        }
        world->AddComponentImmediate(h, light);
    }

    // Optional default physics components for primitive shapes.
    bool withPhysicsBody = false;
    bool withPhysicsCollider = false;
    bool withBoxShape = false;
    bool withSphereShape = false;
    bool withCapsuleShape = false;
    Components::PhysicsBody body{};
    Components::PhysicsCollider col{};
    Components::BoxColliderShape box{};
    Components::SphereColliderShape sphere{};
    Components::CapsuleColliderShape capsule{};

    if (physics != PhysicsSpawn::None)
    {
        withPhysicsBody = true;
        withPhysicsCollider = true;

        // Defaults.
        body.motionType = Physics::MotionType::Dynamic;
        col.layer = Physics::Layers::Dynamic;

        if (physics == PhysicsSpawn::StaticPlaneBox)
        {
            body.motionType = Physics::MotionType::Static;
            col.layer = Physics::Layers::Static;
            withBoxShape = true;
            // Match the visual plane (unit quad) with a thin box so transform scale works.
            box.halfExtentsX = 0.5f;
            box.halfExtentsY = 0.05f;
            box.halfExtentsZ = 0.5f;
        }
        else if (physics == PhysicsSpawn::DynamicBox)
        {
            withBoxShape = true;
            box.halfExtentsX = 0.5f;
            box.halfExtentsY = 0.5f;
            box.halfExtentsZ = 0.5f;
        }
        else if (physics == PhysicsSpawn::DynamicSphere)
        {
            withSphereShape = true;
            sphere.radius = 0.5f;
        }
        else if (physics == PhysicsSpawn::DynamicCapsule)
        {
            withCapsuleShape = true;
            capsule.radius = 0.5f;
            capsule.halfHeight = 0.5f;
            capsule.axis = 1; // Y
        }

        world->AddComponentImmediate(h, body);
        world->AddComponentImmediate(h, col);
        if (withBoxShape)
            world->AddComponentImmediate(h, box);
        if (withSphereShape)
            world->AddComponentImmediate(h, sphere);
        if (withCapsuleShape)
            world->AddComponentImmediate(h, capsule);
    }

    const Components::HierarchyOrder hierarchyOrder = Editor::NextHierarchyOrderAtBottom(world);
    world->AddComponentImmediate(h, hierarchyOrder);

    if (m_Undo)
    {
        auto cmdObj = std::make_unique<CreateEntityCommand>(
            undoName,
            world,
            m_ChangeNotifications,
            h,
            xf,
            nm,
            hasParent,
            parentComp,
            withRenderer,
            mr,
            withCamera,
            cam,
            withLight,
            light,
            withPhysicsBody,
            body,
            withPhysicsCollider,
            col,
            withBoxShape,
            box,
            withSphereShape,
            sphere,
            withCapsuleShape,
            capsule,
            hierarchyOrder);
        m_Undo->CommitAlreadyApplied(std::move(cmdObj));
    }

    // Select after the structure notification refresh has run. Doing this
    // synchronously from key/native-menu command callbacks can re-enter
    // TreeView/SceneView selection wiring while the hierarchy is rebuilding.
    if (hasParent && m_Tree)
    {
        m_Tree->SetExpanded(HierarchyDataProvider::Encode(m_ContextTargetEntity), true);
    }

    NotifyWorldStructure(m_ChangeNotifications, world);
    ScheduleSelectCreatedEntity(h);
}

bool HierarchyPanel::TryHandlePluginCommand(std::uint32_t cmd)
{
    Editor::HierarchyCommandContext pluginCtx{};
    pluginCtx.World = m_World;
    pluginCtx.TargetEntity = m_ContextTargetEntity;
    pluginCtx.HasTargetEntity = m_ContextTargetEntity.IsValid() && m_World && m_World->IsValid(m_ContextTargetEntity);
    pluginCtx.EditorCtx = m_Context;
    pluginCtx.ChangeNotifications = m_ChangeNotifications;
    pluginCtx.Undo = m_Undo;
    pluginCtx.SelectEntity = [this](ECS::EntityHandle entity)
    {
        if (m_OnSelectEntity)
            m_OnSelectEntity(entity);
    };
    pluginCtx.NotifyEntityCreated = [this](ECS::EntityHandle entity)
    {
        if (m_OnEntityCreated)
            m_OnEntityCreated(entity);
    };
    pluginCtx.NotifyWorldStructureChanged = [this]()
    {
        NotifyWorldStructure(m_ChangeNotifications, m_World);
        if (m_Context && m_Context->OnSceneDirty)
            m_Context->OnSceneDirty();
    };
    return Editor::EditorPluginRegistry::Get().HandleHierarchyCommand(cmd, pluginCtx);
}

void HierarchyPanel::CreateEmptyParentForContextTarget()
{
    ECS::World* world = m_World;
    if (!world)
        return;
    if (!m_ContextTargetEntity.IsValid() || !world->IsValid(m_ContextTargetEntity))
        return;

    const ECS::EntityHandle child = m_ContextTargetEntity;

    // Capture the child's current local transform (relative to its current parent).
    Transform childLocal = MakeIdentityTransform();
    if (auto* t = world->GetComponent<Transform>(child))
    {
        childLocal = *t;
    }
    else
    {
        // Best-effort: ensure Transform exists so we can preserve it.
        world->AddComponentImmediate(child, childLocal);
    }

    bool childHadParent = false;
    Parent childParentBefore{};
    if (auto* p = world->GetComponent<Parent>(child))
    {
        childHadParent = true;
        childParentBefore = *p;
    }

    ECS::EntityHandle newParent = world->CreateEntity();
    if (!newParent.IsValid())
        return;

    // New parent inherits the child's previous local transform so its world pose matches.
    world->AddComponentImmediate(newParent, childLocal);
    Name parentName = MakeName("Empty Parent");
    world->AddComponentImmediate(newParent, parentName);
    if (childHadParent)
    {
        // Insert between child's old parent and the child.
        world->AddComponentImmediate(newParent, childParentBefore);
    }

    // Reparent child under new parent and reset child's local transform to identity.
    Parent newParentComp{};
    newParentComp.parent = newParent;
    world->AddComponentImmediate(child, newParentComp);
    world->AddComponentImmediate(child, MakeIdentityTransform());

    if (m_Undo)
    {
        auto cmdObj = std::make_unique<CreateAsEmptyParentCommand>(
            "Create Empty Parent",
            world,
            m_ChangeNotifications,
            child,
            newParent,
            childHadParent,
            childParentBefore,
            childLocal,
            parentName);
        m_Undo->CommitAlreadyApplied(std::move(cmdObj));
    }

    // Select after the hierarchy refresh posted by the structure notification.
    if (m_Tree)
    {
        if (childHadParent && childParentBefore.parent.IsValid())
        {
            m_Tree->SetExpanded(HierarchyDataProvider::Encode(childParentBefore.parent), true);
        }
        m_Tree->SetExpanded(HierarchyDataProvider::Encode(newParent), true);
    }

    NotifyWorldStructure(m_ChangeNotifications, world);
    ScheduleSelectCreatedEntity(newParent);
}

void HierarchyPanel::CreateParticleEmitterEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    const bool hasParent = m_ContextTargetEntity.IsValid() && world->IsValid(m_ContextTargetEntity);
    Parent parentComp{};
    if (hasParent)
        parentComp.parent = m_ContextTargetEntity;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    const bool is2D = m_Context && m_Context->IsSceneView2D && m_Context->IsSceneView2D();
    Transform xf = MakeIdentityTransform();
    Name nm = MakeName("Particle Emitter");
    const Components::ParticleEmitter3D emitter =
        is2D ? Components::MakeParticleEmitter2DDefaults() : Components::ParticleEmitter3D{};
    const Components::HierarchyOrder hierarchyOrder = Editor::NextHierarchyOrderAtBottom(world);

    world->AddComponentImmediate(h, xf);
    world->AddComponentImmediate(h, nm);
    if (hasParent)
        world->AddComponentImmediate(h, parentComp);
    world->AddComponentImmediate(h, emitter);
    world->AddComponentImmediate(h, hierarchyOrder);

    if (m_Undo)
    {
        auto cmdObj = std::make_unique<CreateParticleEmitterEntityCommand>(
            "Create Particle Emitter",
            world,
            m_ChangeNotifications,
            h,
            xf,
            nm,
            hasParent,
            parentComp,
            emitter,
            hierarchyOrder);
        m_Undo->CommitAlreadyApplied(std::move(cmdObj));
    }

    if (hasParent && m_Tree)
        m_Tree->SetExpanded(HierarchyDataProvider::Encode(m_ContextTargetEntity), true);

    NotifyWorldStructure(m_ChangeNotifications, world);
    ScheduleSelectCreatedEntity(h);
}

void HierarchyPanel::CreateReflectionProbeEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    const bool hasParent = m_ContextTargetEntity.IsValid() && world->IsValid(m_ContextTargetEntity);
    Parent parentComp{};
    if (hasParent)
        parentComp.parent = m_ContextTargetEntity;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    Transform xf = Transform::FromTRS(
        Mathematics::Vector3{0.0f, 0.0f, 0.0f},
        Mathematics::Quaternion{},
        Mathematics::Vector3{100.0f, 100.0f, 100.0f});
    Name nm = MakeName("Reflection Probe");
    Components::ReflectionProbe probe{};
    const Components::HierarchyOrder hierarchyOrder = Editor::NextHierarchyOrderAtBottom(world);

    world->AddComponentImmediate(h, xf);
    world->AddComponentImmediate(h, nm);
    if (hasParent)
        world->AddComponentImmediate(h, parentComp);
    world->AddComponentImmediate(h, probe);
    world->AddComponentImmediate(h, hierarchyOrder);

    if (m_Undo)
    {
        auto cmdObj = std::make_unique<CreateReflectionProbeEntityCommand>(
            "Create Reflection Probe",
            world,
            m_ChangeNotifications,
            h,
            xf,
            nm,
            hasParent,
            parentComp,
            probe,
            hierarchyOrder);
        m_Undo->CommitAlreadyApplied(std::move(cmdObj));
    }

    if (hasParent && m_Tree)
        m_Tree->SetExpanded(HierarchyDataProvider::Encode(m_ContextTargetEntity), true);

    NotifyWorldStructure(m_ChangeNotifications, world);
    ScheduleSelectCreatedEntity(h);
}

void HierarchyPanel::CreateOceanEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    world->AddComponentImmediate(h, MakeIdentityTransform());
    world->AddComponentImmediate(h, MakeName("Ocean"));
    world->AddComponentImmediate(h, Editor::NextHierarchyOrderAtBottom(world));
    world->AddComponentImmediate(h, Components::OceanSurface{});
    world->AddComponentImmediate(h, Components::OceanWaveSpectrum{});

    if (m_Selection)
        m_Selection->SetSingle(HierarchyDataProvider::Encode(h));

    NotifyWorldStructure(m_ChangeNotifications, world);

    if (m_OnSelectEntity)
        m_OnSelectEntity(h);
    if (m_OnEntityCreated)
        m_OnEntityCreated(h);

    return;
}

void HierarchyPanel::CreateTerrainEntity(std::uint32_t cmd)
{
    ECS::World* world = m_World;
    if (!world)
        return;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    const CBTTerrainECS::TerrainPreset preset =
        (cmd == HierarchyCommandIds::kCmdCreateTerrainLarge) ? CBTTerrainECS::TerrainPreset::LargePlanar :
        (cmd == HierarchyCommandIds::kCmdCreatePlanet5km)    ? CBTTerrainECS::TerrainPreset::Planet5km :
        (cmd == HierarchyCommandIds::kCmdCreatePlanet50km)   ? CBTTerrainECS::TerrainPreset::Planet50km :
                                          CBTTerrainECS::TerrainPreset::SmallPlanar;
    const Components::Terrain terrainComp = CBTTerrainECS::MakeTerrainPreset(preset);
    const Components::TerrainPlanetRelief reliefComp =
        CBTTerrainECS::MakePlanetReliefPreset(preset);
    const bool spherical = terrainComp.Domain == Components::TerrainDomain::Spherical;

    world->AddComponentImmediate(h, MakeIdentityTransform());
    world->AddComponentImmediate(h, MakeName(spherical ? "Planet" : "Terrain"));
    world->AddComponentImmediate(h, Editor::NextHierarchyOrderAtBottom(world));

    // Shared with the create_terrain / create_entity IPC: adds Terrain + grass,
    // provisions single-tile noise data, gates the heightfield collider to planar
    // single-tile terrains, attaches the base relief to a planet and spawns the
    // scene's default surface rules. The presets are inside every limit it checks.
    if (const std::string refusal = Editor::ProvisionTerrainEntity(*world, h, terrainComp, reliefComp);
        !refusal.empty())
        Logger::Log::Error("HierarchyPanel: the terrain preset was refused: {}", refusal);

    // Select the new terrain entity in the hierarchy.
    if (m_Selection)
        m_Selection->SetSingle(HierarchyDataProvider::Encode(h));

    NotifyWorldStructure(m_ChangeNotifications, world);

    if (m_OnSelectEntity)
        m_OnSelectEntity(h);
    if (m_OnEntityCreated)
        m_OnEntityCreated(h);

    return;
}

void HierarchyPanel::CreateDDGIVolumeEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    // Sized by its transform (DDGIVolume.h), and a unit cube would be a
    // 1 m probe grid — too small to light anything on first placement.
    // Spawn at 10 m per side so a fresh volume covers a room-sized space
    // and is immediately visible in the scene view; the scale gizmo takes
    // it from there.
    constexpr float kDefaultVolumeSize = 10.0f;
    world->AddComponentImmediate(
        h, Transform::FromTRS(Vector3{0.0f, 0.0f, 0.0f}, Quaternion{},
                              Vector3{kDefaultVolumeSize, kDefaultVolumeSize, kDefaultVolumeSize}));
    world->AddComponentImmediate(h, MakeName("DDGI Volume"));
    world->AddComponentImmediate(h, Components::DDGIVolume{});
    world->AddComponentImmediate(h, Editor::NextHierarchyOrderAtBottom(world));

    if (m_Selection)
        m_Selection->SetSingle(HierarchyDataProvider::Encode(h));

    NotifyWorldStructure(m_ChangeNotifications, world);

    if (m_OnSelectEntity)
        m_OnSelectEntity(h);
    if (m_OnEntityCreated)
        m_OnEntityCreated(h);

    return;
}

void HierarchyPanel::CreatePostProcessVolumeEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    world->AddComponentImmediate(h, MakeIdentityTransform());
    world->AddComponentImmediate(h, MakeName("Post Process Volume"));
    world->AddComponentImmediate(h, Components::PostProcessVolume{});
    world->AddComponentImmediate(h, Editor::NextHierarchyOrderAtBottom(world));

    if (m_Selection)
        m_Selection->SetSingle(HierarchyDataProvider::Encode(h));

    NotifyWorldStructure(m_ChangeNotifications, world);

    if (m_OnSelectEntity)
        m_OnSelectEntity(h);
    if (m_OnEntityCreated)
        m_OnEntityCreated(h);

    return;
}

void HierarchyPanel::CreateWindVolumeEntity()
{
    ECS::World* world = m_World;
    if (!world)
        return;

    ECS::EntityHandle h = world->CreateEntity();
    if (!h.IsValid())
        return;

    world->AddComponentImmediate(h, MakeIdentityTransform());
    world->AddComponentImmediate(h, MakeName("Wind Volume"));
    world->AddComponentImmediate(h, Components::WindVolume{});
    world->AddComponentImmediate(h, Editor::NextHierarchyOrderAtBottom(world));

    if (m_Selection)
        m_Selection->SetSingle(HierarchyDataProvider::Encode(h));

    NotifyWorldStructure(m_ChangeNotifications, world);

    if (m_OnSelectEntity)
        m_OnSelectEntity(h);
    if (m_OnEntityCreated)
        m_OnEntityCreated(h);

    return;
}

void HierarchyPanel::ShowContextTargetMeshLocation()
{
    if (!m_ContextTargetEntity.IsValid() || !m_World->IsValid(m_ContextTargetEntity))
        return;
    if (!m_Context || !m_Context->Assets || !m_PingAsset)
        return;
    const GUID modelGuid = Editor::FindModelGuidForEntityOrChildren(m_World, m_ContextTargetEntity);
    if (modelGuid.IsNull())
        return;
    AssetMetadata meta;
    if (!m_Context->Assets->GetRegistry().TryGetAssetMetadata(modelGuid, meta) || meta.Path.empty())
        return;
    m_PingAsset(meta.Path);
    return;
}

void HierarchyPanel::AddSelectionToBookmarks()
{
    if (!m_OnAddEntitiesToBookmarks || !m_World)
        return;
    std::vector<ECS::EntityHandle> entities;
    if (m_Selection)
    {
        for (auto id : m_Selection->GetSelection())
        {
            ECS::EntityHandle h = HierarchyDataProvider::Decode(static_cast<TreeId>(id));
            if (h.IsValid() && m_World->IsValid(h))
                entities.push_back(h);
        }
    }
    if (entities.empty() && m_ContextTargetEntity.IsValid() && m_World->IsValid(m_ContextTargetEntity))
        entities.push_back(m_ContextTargetEntity);
    if (!entities.empty())
        m_OnAddEntitiesToBookmarks(entities);
    return;
}

HierarchyPanel::~HierarchyPanel()
{
    {
        std::lock_guard<std::mutex> lock(HierarchyPanelRegistryMutex());
        HierarchyPanelRegistry().erase(this);
    }
    m_Alive->store(false, std::memory_order_release);
    m_RefreshQueued = false;
    ClearSearchHighlights();
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);

    if (m_ChangeNotifications && m_StructureSub)
    {
        m_ChangeNotifications->Unsubscribe(m_StructureSub);
    }
    if (m_ChangeNotifications && m_LightChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_LightChangedSub);
    }
    if (m_ChangeNotifications && m_MeshRendererChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_MeshRendererChangedSub);
    }
    if (m_ChangeNotifications && m_SkinnedMeshRendererChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_SkinnedMeshRendererChangedSub);
    }
    if (m_ChangeNotifications && m_RenderLayerChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_RenderLayerChangedSub);
    }
    if (m_ChangeNotifications && m_NameChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_NameChangedSub);
    }
}

void HierarchyPanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    m_Window = ctx ? ctx->MainWindow : nullptr;
    if (m_VcsController)
        m_VcsController->SetContext(ctx);
}

void HierarchyPanel::SetChangeNotifications(Editor::EditorChangeNotifications* notifications)
{
    if (m_ChangeNotifications && m_StructureSub)
    {
        m_ChangeNotifications->Unsubscribe(m_StructureSub);
    }
    if (m_ChangeNotifications && m_LightChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_LightChangedSub);
    }
    if (m_ChangeNotifications && m_MeshRendererChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_MeshRendererChangedSub);
    }
    if (m_ChangeNotifications && m_SkinnedMeshRendererChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_SkinnedMeshRendererChangedSub);
    }
    if (m_ChangeNotifications && m_RenderLayerChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_RenderLayerChangedSub);
    }
    if (m_ChangeNotifications && m_NameChangedSub)
    {
        m_ChangeNotifications->Unsubscribe(m_NameChangedSub);
    }

    m_ChangeNotifications = notifications;
    m_StructureSub = {};
    m_LightChangedSub = {};
    m_MeshRendererChangedSub = {};
    m_SkinnedMeshRendererChangedSub = {};
    m_RenderLayerChangedSub = {};
    m_NameChangedSub = {};

    if (!m_ChangeNotifications)
    {
        return;
    }

    m_StructureSub = m_ChangeNotifications->SubscribeWorldStructureChanged(
        [this](const Editor::EditorChangeNotifications::WorldStructureChangedEvent& e)
        {
            if (e.world != m_World)
            {
                return;
            }

            // Always defer refresh to avoid re-entrancy while the world is mid-mutation (e.g. World::Clear()).
            // Coalesce multiple notifications into a single refresh.
            if (m_RefreshQueued)
                return;
            m_RefreshQueued = true;
            const bool restoreHierarchyUiFromScene = (e.kind == Editor::EditorChangeNotifications::ChangeKind::Commit);
            this->PostAction([this, restoreHierarchyUiFromScene]()
                             {
                                 m_RefreshQueued = false;
                                 this->Refresh(restoreHierarchyUiFromScene); });
        });

    // Live hierarchy icon tint: follow color-picker drags on Light.Color.
    const ECS::ComponentTypeId lightTypeId = ECS::GetComponentTypeId<Components::Light>();
    m_LightChangedSub = m_ChangeNotifications->SubscribeComponentChanged(
        [this, lightTypeId](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.world != m_World || e.componentType != lightTypeId)
                return;
            RefreshLightTintForEntity(e.entity);
        });

    // Refresh sprite/model thumbnails when MeshRenderer changes (e.g. texture drop on mesh).
    const ECS::ComponentTypeId meshRendererTypeId = ECS::GetComponentTypeId<Components::MeshRenderer>();
    m_MeshRendererChangedSub = m_ChangeNotifications->SubscribeComponentChanged(
        [this, meshRendererTypeId](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.world != m_World || e.componentType != meshRendererTypeId)
                return;
            if (m_RefreshQueued)
                return;
            m_RefreshQueued = true;
            this->PostAction([this]()
                             {
                                 m_RefreshQueued = false;
                                 this->Refresh(false); });
        });

    const auto refreshRenderLayerLabel = [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
    {
        if (e.world != m_World || !m_LayerControls.IsRenderLayerVisible() || m_RefreshQueued)
            return;
        m_RefreshQueued = true;
        this->PostAction([this]()
                         {
                             m_RefreshQueued = false;
                             this->Refresh(false); });
    };
    const ECS::ComponentTypeId skinnedMeshRendererTypeId = ECS::GetComponentTypeId<Components::SkinnedMeshRenderer>();
    m_SkinnedMeshRendererChangedSub = m_ChangeNotifications->SubscribeComponentChanged(
        [refreshRenderLayerLabel](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.componentType == skinnedMeshRendererTypeId)
                refreshRenderLayerLabel(e);
        });
    const ECS::ComponentTypeId renderLayerTypeId = ECS::GetComponentTypeId<Components::RenderLayer>();
    m_RenderLayerChangedSub = m_ChangeNotifications->SubscribeComponentChanged(
        [refreshRenderLayerLabel](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.componentType == renderLayerTypeId)
                refreshRenderLayerLabel(e);
        });

    // Label text comes from Name; inspector edits notify Commit without always sending
    // WorldStructureChanged (e.g. while typing). Incremental path: this is Tier 1 — update the
    // one label + Subset rebind (ApplyRenamed), no full rebuild. Legacy path (kill switch OFF):
    // the coalesced full label rebuild, byte-identical to before.
    const ECS::ComponentTypeId nameTypeId = ECS::GetComponentTypeId<Components::Name>();
    m_NameChangedSub = m_ChangeNotifications->SubscribeComponentChanged(
        [this, nameTypeId](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.world != m_World || e.componentType != nameTypeId)
                return;
            if (e.kind == Editor::EditorChangeNotifications::ChangeKind::Preview)
                return;
            if (HierarchyIncrementalEnabled())
            {
                // Defer (avoid re-entrancy while the world is mid-mutation), then apply the
                // per-entity rename. Idempotent with the Changed<Name> scan lane.
                const ECS::EntityHandle entity = e.entity;
                this->PostAction([this, entity]()
                                 {
                                     if (m_Tree && m_Provider)
                                     {
                                         m_Provider->ApplyRenamed(entity);
                                         m_Provider->FlushPendingResorts(); // single rename: resort now
                                         if (!m_CurrentSearchText.empty())
                                         {
                                             const std::string query = m_CurrentSearchText;
                                             ApplySearchFilter(query);
                                         }
                                     }
                                 });
                return;
            }
            // Legacy: coalesced full label rebuild (bypasses m_RefreshQueued so it never swallows
            // a pending structure refresh; m_NameRebuildQueued keeps Update()'s Changed<Name> scan
            // from scheduling a second rebuild for the same commit).
            if (m_NameRebuildQueued)
                return;
            m_NameRebuildQueued = true;
            this->PostAction([this]()
                             {
                                 m_NameRebuildQueued = false;
                                 if (!m_Tree || !m_Provider)
                                     return;
                                 ArmValueChangeGate();
                                 m_Provider->Rebuild();
                                 m_Tree->RefreshFromProvider(); });
        });
}

void HierarchyPanel::ApplyGeneratedRowPreview(
    UIElement* row, TreeId id, UIElement* previewIconEl,
    const std::function<std::string(std::function<void(const std::string&)>)>& request)
{
    const auto weakPanel = UIElement::MakeWeakRef(this);
    const auto weakRow = UIElement::MakeWeakRef(row);
    const auto weakIcon = UIElement::MakeWeakRef(previewIconEl);
    const std::string immediate = request(
        [weakPanel, weakRow, weakIcon, id](const std::string& rel)
        {
            if (rel.empty())
                return;
            // The concrete thumbnail service delivers this callback on the UI
            // thread; the panel can have closed while the worker produced it.
            auto* panel = weakPanel.Get();
            if (!panel)
                return;
            panel->PostAction(
                [weakPanel, weakRow, weakIcon, id, rel]()
                {
                    auto* currentPanel = weakPanel.Get();
                    auto* currentRow = weakRow.Get();
                    auto* currentIcon = weakIcon.Get();
                    if (!currentPanel || !currentRow || !currentIcon || !currentPanel->m_Tree)
                        return;
                    if (currentPanel->m_Context && currentPanel->m_Context->UIReplayActive)
                        return;
                    // TreeView owns pooled-row bindings. A live row may now
                    // represent another entity even when its icon still exists.
                    if (currentPanel->m_Tree->BoundIdOf(currentRow) != id)
                        return;
                    Editor::ApplySpriteFrameThumbnail(*currentIcon, rel, kHierarchyRowIconAlignPercent);
                });
        });
    if (!immediate.empty())
        Editor::ApplySpriteFrameThumbnail(*previewIconEl, immediate, kHierarchyRowIconAlignPercent);
}

void HierarchyPanel::ApplyModelThumbColorOverride(UIElement* titleEl)
{
    if (!titleEl || !titleEl->HasClass("hierarchy-entity-model-thumb"))
        return;
    UIManager* mgr = titleEl->GetOwnerManager();
    if (!mgr)
        return;
    UIElement* root = mgr->GetRootElement();
    if (!root)
        return;
    const bool modelsColored = root->HasClass("hierarchy-model-thumbs-colored") ||
                               root->HasClass("hierarchy-icons-colored");
    if (!modelsColored)
        return;
    titleEl->Overrides().Set(Style::BackgroundImageSaturation, 1.0f);
    titleEl->Overrides().Set(Style::BackgroundTint, uint32_t(0xFFFFFFFFu));
    titleEl->MarkDirty(UIElement::VisualDirty);
}

void HierarchyPanel::ApplyColoredIconsMode(bool colored)
{
    UIManager* mgr = GetOwnerManager();
    if (!mgr)
        return;

    // Keep the root class in sync so CSS rules (.hierarchy-icons-colored …) stay matched,
    // and so the light tint applied at the next bind reads the correct mode.
    if (UIElement* root = mgr->GetRootElement())
    {
        if (colored)
            root->AddClass("hierarchy-icons-colored");
        else
            root->RemoveClass("hierarchy-icons-colored");
        mgr->MarkStyleDirtySubtree(root);
    }

    // Inline BackgroundTint overrides on already-bound rows dominate CSS; rebind each one
    // so the new mode is visible immediately without requiring hierarchy focus or a scroll.
    RePresentHeldRows();
}

void HierarchyPanel::ApplyModelThumbsAlwaysColoredMode(bool colored)
{
    UIManager* mgr = GetOwnerManager();
    if (!mgr)
        return;

    if (UIElement* root = mgr->GetRootElement())
    {
        if (colored)
            root->AddClass("hierarchy-model-thumbs-colored");
        else
            root->RemoveClass("hierarchy-model-thumbs-colored");
        mgr->MarkStyleDirtySubtree(root);
    }

    // Rebind every bound row: the bind runs the light-tint logic (which resets the inline
    // overrides based on the Colored Hierarchy Icons flag) and the model-thumb override, so rows
    // with the model-thumb class end up colored when either flag is on.
    RePresentHeldRows();
}

void HierarchyPanel::NotifyColoredIconsChanged(bool colored)
{
    std::vector<HierarchyPanel*> snap;
    {
        std::lock_guard<std::mutex> lock(HierarchyPanelRegistryMutex());
        snap.reserve(HierarchyPanelRegistry().size());
        for (auto* p : HierarchyPanelRegistry())
            snap.push_back(p);
    }
    for (auto* p : snap)
    {
        if (p)
            p->ApplyColoredIconsMode(colored);
    }
}

void HierarchyPanel::NotifyModelThumbsAlwaysColoredChanged(bool colored)
{
    std::vector<HierarchyPanel*> snap;
    {
        std::lock_guard<std::mutex> lock(HierarchyPanelRegistryMutex());
        snap.reserve(HierarchyPanelRegistry().size());
        for (auto* p : HierarchyPanelRegistry())
            snap.push_back(p);
    }
    for (auto* p : snap)
    {
        if (p)
            p->ApplyModelThumbsAlwaysColoredMode(colored);
    }
}

void HierarchyPanel::RefreshLightTintForEntity(ECS::EntityHandle handle)
{
    if (!handle.IsValid())
        return;
    if (!m_Tree || !m_World)
        return;
    Editor::RetintHeldHierarchyLightRow(*m_World, handle, *m_Tree, HierarchyDataProvider::Encode(handle));
}

void HierarchyPanel::RePresentHeldRows()
{
    if (!m_Provider || !m_Tree)
        return;
    std::vector<TreeId> held;
    m_Tree->CollectBoundIds(held);
    m_Provider->MarkChangedBatch(held);
    m_Tree->RefreshFromProvider();
}

std::vector<ECS::EntityHandle> HierarchyPanel::ResolveRowActionTargets(
    ECS::EntityHandle clicked) const
{
    std::vector<ECS::EntityHandle> roots;
    if (!m_World || !clicked.IsValid() || !m_World->IsValid(clicked))
        return roots;

    roots.push_back(clicked);
    if (!m_Selection)
        return roots;

    const std::vector<UI::Interaction::ItemId> selectedIds = m_Selection->GetSelection();
    const UI::Interaction::ItemId clickedId = static_cast<UI::Interaction::ItemId>(
        HierarchyDataProvider::Encode(clicked));
    if (selectedIds.size() <= 1 ||
        std::find(selectedIds.begin(), selectedIds.end(), clickedId) == selectedIds.end())
        return roots;

    roots.clear();
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(selectedIds.size());
    for (UI::Interaction::ItemId selectedId : selectedIds)
    {
        const TreeId treeId = static_cast<TreeId>(selectedId);
        if (treeId == 0 || treeId == kRootId)
            continue;
        const ECS::EntityHandle entity = HierarchyDataProvider::Decode(treeId);
        if (!entity.IsValid() || !m_World->IsValid(entity))
            continue;
        if (seen.insert(entity.id).second)
            roots.push_back(entity);
    }
    if (roots.empty())
        roots.push_back(clicked);
    return roots;
}

void HierarchyPanel::SetWorld(ECS::World* world)
{
    m_World = world;
    // Seed the poll baseline to the world we're about to rebuild against, so the first Update()
    // after a world swap doesn't spuriously rebuild an already-current provider. The change gate
    // needs the same seeding: version streams are per-World, so a gate carried across a swap
    // would compare against the wrong stream (a stale high value silently filters everything).
    m_LastWorldStructuralVersion = world ? world->GetStructuralChangeVersion() : 0;
    ArmValueChangeGate();
    ReseedIncrementalBaselines();
    if (!m_Tree)
        return;
    if (!m_Provider)
        m_Provider = std::make_unique<HierarchyDataProvider>(world);
    else
        m_Provider->SetWorld(world);
    if (m_Selection)
        m_Selection->Clear();
    m_Tree->SetDataProvider(m_Provider.get());
    this->PostAction([this]()
                     {
                         if (m_Provider)
                         {
                             ArmValueChangeGate();
                             m_Provider->Rebuild();
                         }
                         if (m_Tree)
                         {
                             RefreshSceneDiffDecorations();
                         }
                         if (!m_CurrentSearchText.empty())
                         {
                             const std::string query = m_CurrentSearchText;
                             ApplySearchFilter(query);
                     } });
}

static_assert(kRootId == Editor::kHierarchyRootTreeId,
              "ghost row parenting relies on the same root sentinel");

void HierarchyPanel::SetGhostRows(std::vector<GhostRow> rows)
{
    if (!m_Provider)
        return;
    std::vector<HierarchyDataProvider::GhostRow> providerRows;
    providerRows.reserve(rows.size());
    for (GhostRow& row : rows)
        providerRows.push_back({row.Id, row.ParentId, std::move(row.Label)});
    m_Provider->SetGhostRows(std::move(providerRows));
}

TreeId HierarchyPanel::EntityTreeId(const ECS::EntityHandle& entity)
{
    return HierarchyDataProvider::Encode(entity);
}

void HierarchyPanel::SetSceneDiffProvider(
    std::function<std::vector<Editor::SceneObjectDiff>()> provider)
{
    if (m_VcsController)
        m_VcsController->SetSceneDiffProvider(std::move(provider));
}

void HierarchyPanel::SetScenePathProvider(
    std::function<std::optional<std::filesystem::path>()> provider)
{
    if (m_VcsController)
        m_VcsController->SetScenePathProvider(std::move(provider));
}

void HierarchyPanel::SetSceneDiffIndicatorToggle(std::function<void()> toggle)
{
    if (m_VcsController)
        m_VcsController->SetIndicatorVisibilityToggle(std::move(toggle));
}

void HierarchyPanel::RefreshSceneDiffDecorations()
{
    if (m_VcsController)
        m_VcsController->Refresh();
}

void HierarchyPanel::ArmValueChangeGate()
{
    m_ValueChangeGate.LastRunVersion = m_World ? m_World->GetGlobalSystemVersion() : 0;
}

void HierarchyPanel::Update()
{
    if (!m_World || !m_Provider || !m_Tree)
        return;

    if (m_LayerControls.NeedsRefresh(*m_World))
        RePresentHeldRows();
    m_RowActivity.Refresh(*m_World, *m_Tree, *m_Provider);

    if (m_VcsController)
        m_VcsController->Update();

    // Kill switch (design §5): OFF restores the pre-incremental full-Refresh path exactly.
    if (!HierarchyIncrementalEnabled())
    {
        UpdateLegacyFullRebuild();
        return;
    }

    // Review F4: the incremental path is load-bearing on Removed<Parent> lifecycle
    // events — a Parent-component remove is invisible to both the identity diff
    // (membership unchanged) and the Changed<Parent> scan (the entity left the
    // Parent archetype) — and on the swap-generation gap-heal. Both are silently
    // dead on a world that never called EnableLifecycleEvents<Parent> (registration
    // is primary-world-only, Engine.cpp), so take the always-correct legacy path
    // there. Evaluated per Update: the bound world can swap.
    if (!m_World->IsLifecycleEventsEnabledFor(ECS::GetComponentTypeId<Components::Parent>()))
    {
        UpdateLegacyFullRebuild();
        return;
    }

    // Poll the world's structural-change version. Advance the baseline regardless of which
    // path services the change (mirrors the legacy path).
    bool structuralMoved = false;
    const std::size_t worldVersion = m_World->GetStructuralChangeVersion();
    if (worldVersion != m_LastWorldStructuralVersion)
    {
        m_LastWorldStructuralVersion = worldVersion;
        structuralMoved = true;
    }

    // LifecycleEvents Q4 (World::Clear / scene swap): reset-generation change ⇒ full Rebuild.
    // A scene load fires a Commit notification too, so if a full Refresh is already queued let it
    // service the swap (avoids a double rebuild); just advance the reset baseline so we don't
    // re-trigger — the queued Refresh reseeds every incremental baseline.
    if (m_World->GetLifecycleResetGeneration() != m_LastLifecycleResetGen)
    {
        if (m_RefreshQueued)
        {
            m_LastLifecycleResetGen = m_World->GetLifecycleResetGeneration();
            return;
        }
        Refresh(false);
        return;
    }

    // A deferred scene build resolves entities across frames, bumping the world's
    // structural version EVERY frame (the pump AddComponentImmediate's mesh/skeleton/
    // bounds/WorldTransform components). Servicing that here would re-run the
    // incremental diff + re-request every visible row's model thumbnail each frame
    // over the whole 21k-entity scene — starving the pump and doubling load
    // wall-clock. Hold the per-frame incremental churn while a build runs. This sits
    // AFTER the reset-generation rebuild above, so the world SWAP still rebuilds the
    // tree once (the new scene's rows appear immediately, not the previous scene's);
    // only the topology-irrelevant per-frame render-component adds are deferred. The
    // build's completion fires a structural Commit that reconciles the tree.
    if (m_Context && m_Context->SceneBuildProgress)
    {
        uint64_t processed = 0;
        uint64_t total = 0;
        if (m_Context->SceneBuildProgress(processed, total))
            return;
    }

    // Cadence guard: a swap-generation gap > 1 means at least one lifecycle-event window was
    // discarded unseen (panel hidden — EditorPanelManager skips zero-dim panels — or throttled).
    // A parent-component REMOVE that happened in a missed window is not rediscoverable by the
    // identity diff (membership unchanged) nor the Changed<Parent> scan (the entity left the
    // Parent archetype), so the safe heal is the always-correct full Rebuild (design §2
    // "gap > 1 ⇒ full rebuild").
    //
    // Deliberately NOT ECS::SwapGenerationGuard (compare-then-assign, consumed once per run at
    // entry): here the baseline is owned by the heal, not by the check.
    //   - The miss path does not assign — Refresh() reseeds via ReseedIncrementalBaselines()
    //     only once the Rebuild has re-established provider truth, so a gap keeps re-detecting
    //     until a Rebuild actually lands (sticky miss until healed).
    //   - The earlier returns (kill switch, lifecycle-disabled world, reset-generation deferral)
    //     skip this check on purpose: the gap accrued across those frames is what turns re-entry
    //     into the gap>1 full-Rebuild heal.
    //   - SetWorld() seeds the baseline to the new world's current generation (its posted Rebuild
    //     is the world-switch recovery); the shared guard's world pairing would report a miss and
    //     force a second full rebuild on every world swap.
    const uint64 swapGen = m_World->GetLifecycleSwapGeneration();
    if (swapGen - m_LastLifecycleSwapGen > 1)
    {
        Refresh(false);
        return;
    }
    m_LastLifecycleSwapGen = swapGen;

    // Editor command paths own their own deferred full Refresh (it carries selection/scene-UI
    // semantics), so defer to them exactly as the legacy path did. The pending Refresh re-seeds
    // all incremental baselines.
    if (m_RefreshQueued)
        return;

    // --- Incremental maintenance (design §2 Tier 1+2) ---
    // Tracks whether any lane touched provider state this frame; gates the Debug parity check
    // so idle frames don't pay a full Rebuild (unused outside Debug).
    [[maybe_unused]] bool incrementalTouched = structuralMoved;

    // Entity create/destroy: the identity diff is the authority (no entity-level events exist)
    // and the correctness backstop.
    bool anyDestroyed = false;
    if (structuralMoved)
    {
        const auto diff = m_Provider->ApplyIdentityDiff();
        anyDestroyed = diff.AnyDestroyed;
        // diff.Escalate: F1 mass spawn/despawn (delta > kIncrementalDeltaMax) — the diff declined
        //   to apply; full rebuild is the correct fallback.
        // !diff.AnyMembershipChange: F3(a) — the structural version moved but no entity was
        //   created/destroyed, so the change was a component add/remove on existing entities
        //   (HierarchyOrder add, Name remove, type/icon components, a pure Parent add/remove
        //   frame). No incremental lane recomputes that derived state; a full Refresh restores
        //   the legacy healing for the whole derived-state family.
        if (diff.Escalate || !diff.AnyMembershipChange)
        {
            Refresh(false);
            return;
        }
    }

    // Reparent-move budget (F1): a no-op ApplyReparented is O(1), but each real move is an
    // O(sibling-list) insert+erase — a mass reparent would be O(N^2). Count real moves; past the
    // threshold, stop applying and full-rebuild. Chunk-granular over-reports (no-ops) don't count.
    std::size_t reparentMoves = 0;
    bool escalateReparent = false;
    const ECS::ComponentTypeId parentTypeId = ECS::GetComponentTypeId<Components::Parent>();
    auto tryReparent = [&](ECS::EntityHandle e)
    {
        if (escalateReparent)
            return;
        incrementalTouched = true;
        if (m_Provider->ApplyReparented(e) && ++reparentMoves > kIncrementalDeltaMax)
            escalateReparent = true;
    };

    // Reparent by Parent component add/remove: Removed<Parent> is the only INCREMENTAL detector
    // for a parent-remove (the entity leaves the Parent archetype so the Changed<Parent> scan
    // below can't see it, and the diff sees no membership change). It matters on mixed frames
    // (a membership delta present, so F3(a) above didn't escalate); a pure parent add/remove
    // frame is already caught by the F3(a) full Refresh. Added<Parent> is redundant with the scan
    // but idempotent. A destroy's signature-Removed no-ops (entity no longer tracked).
    for (ECS::EntityHandle e : m_World->GetRemoved(parentTypeId))
        tryReparent(e);
    for (ECS::EntityHandle e : m_World->GetAdded(parentTypeId))
        tryReparent(e);
    if (escalateReparent)
    {
        Refresh(false); // reseeds gate + baselines
        return;
    }

    // Value-write lanes (data-only Set<Parent> / Set<Name> / Set<HierarchyOrder> — the #390 gate):
    // the chunk column is stamped but no structural bump and no notification fire, so only these
    // read-only Changed<> scans catch them. All three share m_ValueChangeGate against last frame's
    // baseline; re-arm once at the end (ChangeGate contract: entry-sample before reading,
    // write-back after) — this is the incremental path's gate re-arm.
    if (ECS::ChangeFilter::Enabled())
    {
        const uint64 entryVersion = m_World->GetGlobalSystemVersion();

        // F3(b): HierarchyOrder writes (data-only, or a component add caught via the dest chunk's
        // full-stamp) reorder siblings, but no incremental lane recomputes order — full Refresh
        // heals it (rare in gameplay; correctness first). Detect-only, like the legacy scans.
        bool orderChanged = false;
        {
            // The hierarchy lists every entity, so every scan behind it opts
            // back into the rows the enable model hides.
            auto scan = m_World->Query<ECS::Read<Components::HierarchyOrder>>();
            scan.IncludeDisabled();
            scan.Changed<Components::HierarchyOrder>(m_ValueChangeGate);
            scan.BatchEach([&](const Components::HierarchyOrder*, std::size_t) { orderChanged = true; });
        }
        if (orderChanged)
        {
            Refresh(false); // reseeds gate + baselines
            return;
        }

        {
            auto scan = m_World->Query<ECS::Read<Components::Parent>>();
            scan.IncludeDisabled();
            scan.Changed<Components::Parent>(m_ValueChangeGate);
            scan.Each([&](ECS::EntityHandle e, const Components::Parent&) { tryReparent(e); });
        }
        if (escalateReparent)
        {
            Refresh(false); // reseeds gate + baselines
            return;
        }
        {
            auto scan = m_World->Query<ECS::Read<Components::Name>>();
            scan.IncludeDisabled();
            scan.Changed<Components::Name>(m_ValueChangeGate);
            scan.Each([&](ECS::EntityHandle e, const Components::Name&)
                      { incrementalTouched = true; m_Provider->ApplyRenamed(e); });
        }
        m_ValueChangeGate.LastRunVersion = entryVersion;
    }

    // Coalesced rename resorts (F2): sort each sibling list a rename touched exactly once, after
    // all rename candidates are processed (so N renames into one list cost one SortGroup).
    m_Provider->FlushPendingResorts();

    if (reparentMoves > 0)
    {
        if (m_VcsController)
            m_VcsController->RefreshLiveState();
        else
            m_Tree->RefreshFromProvider();
    }

#if !defined(NDEBUG) && !defined(GE_DEBUGFAST)
    // Behavioral-parity assertion (Debug config only — see the provider method). Only after a
    // batch that touched provider state, so idle frames don't rebuild-per-frame.
    if (incrementalTouched)
        m_Provider->DebugVerifyAgainstRebuild();
#endif

    // Selection revalidation for vanished rows: route through the same helper Refresh() uses,
    // under the same undo-suppression flag (a delete-driven selection change must not record its
    // own selection-undo — the triggering edit already owns the undo step).
    if (anyDestroyed)
    {
        ScopedFlag suppress(m_SuppressSelectionUndo);
        RevalidateAndSyncSelection();
    }
    if (incrementalTouched && !m_CurrentSearchText.empty())
    {
        const std::string query = m_CurrentSearchText;
        ApplySearchFilter(query);
    }
}

void HierarchyPanel::UpdateLegacyFullRebuild()
{
    bool structuralMoved = false;
    const std::size_t worldVersion = m_World->GetStructuralChangeVersion();
    if (worldVersion != m_LastWorldStructuralVersion)
    {
        // The world's entity set changed since the last rebuild. Advance the baseline regardless
        // of which path services the refresh (see the dispatch below).
        m_LastWorldStructuralVersion = worldVersion;
        structuralMoved = true;
    }
    bool needsRefresh = structuralMoved;

    // Value-write scan: a gameplay/script Set<Parent> on an already-parented entity (or
    // Set<Name> on a named one) takes the ECS data-only fast path — the chunk column is
    // stamped, but no structural version bump and no EditorChangeNotifications ever fire,
    // so the poll above can't see it. The read-only Changed<> scans visit only stamped
    // chunks: a clean frame costs one uint64 compare per Parent/Name chunk and no entity
    // touches. Skipped when a structural refresh is already due — Refresh() re-arms the
    // gate, consuming any value stamps along with it.
    if (!needsRefresh && ECS::ChangeFilter::Enabled())
    {
        const uint64 entryVersion = m_World->GetGlobalSystemVersion();
        bool valueChanged = false;
        {
            auto scan = m_World->Query<ECS::Read<Components::Parent>>();
            scan.IncludeDisabled();
            scan.Changed<Components::Parent>(m_ValueChangeGate);
            scan.BatchEach([&](const Components::Parent*, std::size_t) { valueChanged = true; });
        }
        if (!valueChanged)
        {
            auto scan = m_World->Query<ECS::Read<Components::Name>>();
            scan.IncludeDisabled();
            scan.Changed<Components::Name>(m_ValueChangeGate);
            scan.BatchEach([&](const Components::Name*, std::size_t) { valueChanged = true; });
        }
        // Entry-sample write-back either way (ChangeGate contract): if nothing changed, no
        // stamp exists in (gate, entry]; if a refresh runs below, it consumes everything up
        // to its own entry and stamps landing during it compare greater next frame.
        m_ValueChangeGate.LastRunVersion = entryVersion;
        needsRefresh = valueChanged;
    }

    if (!needsRefresh)
        return;
    // If an EditorChangeNotifications refresh is already queued (the mutation came through the
    // editor command layer), let that deferred Refresh service it against the live world;
    // otherwise the mutation arrived from a path that doesn't notify (IPC bulk spawn, scripts,
    // gameplay), so rebuild here.
    if (m_RefreshQueued)
        return;
    // A pending Name-label rebuild may only absorb scan-driven refreshes: it is a bare provider
    // rebuild without Refresh()'s selection revalidation and undo suppression, which structural
    // changes (e.g. a non-notifying delete landing in the same frame as a rename commit) require.
    if (m_NameRebuildQueued && !structuralMoved)
        return;
    Refresh(false);
}

void HierarchyPanel::ReseedIncrementalBaselines()
{
    if (!m_World)
    {
        m_LastLifecycleResetGen = 0;
        m_LastLifecycleSwapGen = 0;
        return;
    }
    m_LastLifecycleResetGen = m_World->GetLifecycleResetGeneration();
    m_LastLifecycleSwapGen = m_World->GetLifecycleSwapGeneration();
}

void HierarchyPanel::Refresh(bool restoreHierarchyUiFromSceneAfterCommit)
{
    if (!m_Tree || !m_Provider)
        return;
    // A world-change-driven refresh may drop the selected row (e.g. after a delete) or
    // otherwise re-sync selection. That selection change is a consequence of the world
    // edit, not a user selection action, so it must not record its own selection-undo
    // command — otherwise the triggering edit (delete/undo/redo) needs an extra Ctrl+Z.
    // This refresh is deferred (PostAction from the world-structure-changed notify), so a
    // synchronous suppress at the call site can't cover it; guard the whole rebuild here.
    // ScopedFlag restores even if a callee throws, so a mid-refresh bad_alloc can't leave
    // selection-undo permanently suppressed.
    ScopedFlag suppress(m_SuppressSelectionUndo);
    // This rebuild reads the current world state, so consume pending Parent/Name value
    // stamps up front — Update()'s change scan must not schedule another refresh for
    // anything at-or-before this point.
    ArmValueChangeGate();
    m_Provider->Rebuild();
    // A full Rebuild re-establishes provider truth against the current world, so re-seed the
    // incremental baselines to match (kill switch OFF keeps the legacy path byte-identical —
    // those baselines are never read there). The provider re-seeds its own position index inside
    // Rebuild; this covers the panel-side generation/version baselines.
    if (HierarchyIncrementalEnabled())
    {
        m_LastWorldStructuralVersion = m_World ? m_World->GetStructuralChangeVersion() : 0;
        ReseedIncrementalBaselines();
    }
    // Full VCS refresh, not just a tree repaint: a structural commit — undo
    // and redo included — changes which baseline-only entities are live, and
    // ghost rows are only re-evaluated by ReloadDiffCache. The diff itself is
    // memoized on file identity, so this costs no provider process.
    if (m_VcsController)
        m_VcsController->Refresh();
    if (restoreHierarchyUiFromSceneAfterCommit)
        RestoreHierarchyUiFromSceneAfterSceneCommit();

    if (m_PlayModeSelectionStash.has_value())
        RestoreStashedSelectionAfterPlayModeTransition();
    else
        RevalidateAndSyncSelection();

    if (!m_CurrentSearchText.empty())
    {
        const std::string query = m_CurrentSearchText;
        ApplySearchFilter(query);
    }
}

std::optional<HierarchyPanel::PlayModeSelectionStash> HierarchyPanel::CaptureSelectionForPlayModeTransition() const
{
    if (!m_Selection || !m_World)
        return std::nullopt;

    PlayModeSelectionStash stash{};
    const std::vector<UI::Interaction::ItemId> sel = m_Selection->GetSelection();
    if (sel.empty())
        return std::nullopt;

    const TreeId anchorTid = static_cast<TreeId>(m_Selection->GetAnchor());
    ECS::EntityHandle anchorHandle = HierarchyDataProvider::Decode(anchorTid);
    std::string anchorTag;
    if (anchorHandle.IsValid() && m_World->IsValid(anchorHandle))
    {
        anchorTag = GetEntitySceneTagString(*m_World, anchorHandle);
        stash.EntitiesOrdered.push_back(anchorHandle);
    }

    std::unordered_set<std::string> seenTags;
    if (!anchorTag.empty())
    {
        stash.SceneTagsOrdered.push_back(anchorTag);
        seenTags.insert(anchorTag);
    }

    std::unordered_set<std::uint32_t> seenEntityIds;
    if (anchorHandle.IsValid())
        seenEntityIds.insert(anchorHandle.id);

    for (auto itemId : sel)
    {
        const TreeId tid = static_cast<TreeId>(itemId);
        if (tid == 0 || tid == kRootId)
            continue;

        ECS::EntityHandle h = HierarchyDataProvider::Decode(tid);
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;

        if (seenEntityIds.insert(h.id).second)
            stash.EntitiesOrdered.push_back(h);

        std::string tag = GetEntitySceneTagString(*m_World, h);
        if (tag.empty() || seenTags.count(tag) != 0)
            continue;
        seenTags.insert(tag);
        stash.SceneTagsOrdered.push_back(std::move(tag));
    }

    if (stash.EntitiesOrdered.empty() && stash.SceneTagsOrdered.empty())
        return std::nullopt;

    return stash;
}

void HierarchyPanel::StashSelectionForPlayModeTransition()
{
    m_PlayModeSelectionStash = CaptureSelectionForPlayModeTransition();
}

void HierarchyPanel::ApplySelectionFromPlayModeStash(const PlayModeSelectionStash& stash)
{
    if (!m_Selection || !m_Tree || !m_World || !m_Provider)
        return;

    std::vector<ECS::EntityHandle> resolved;
    resolved.reserve(stash.EntitiesOrdered.size());

    std::unordered_set<std::uint32_t> seen;
    for (const std::string& tag : stash.SceneTagsOrdered)
    {
        if (tag.empty())
            continue;
        ECS::EntityHandle h = FindEntityBySceneTag(*m_World, tag);
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;
        if (seen.insert(h.id).second)
            resolved.push_back(h);
    }

    if (resolved.empty())
    {
        for (ECS::EntityHandle h : stash.EntitiesOrdered)
        {
            if (!h.IsValid() || !m_World->IsValid(h))
                continue;
            if (seen.insert(h.id).second)
                resolved.push_back(h);
        }
    }

    if (resolved.empty())
        return;

    if (resolved.size() == 1)
    {
        SyncSelectionWithSceneViewPick(resolved.front());
        return;
    }

    SelectEntities(resolved);
}

void HierarchyPanel::RestoreStashedSelectionAfterPlayModeTransition()
{
    if (!m_PlayModeSelectionStash.has_value())
        return;

    const PlayModeSelectionStash stash = std::move(*m_PlayModeSelectionStash);
    m_PlayModeSelectionStash.reset();
    ApplySelectionFromPlayModeStash(stash);
}

void HierarchyPanel::RevalidateAndSyncSelection()
{
    if (!m_Selection || !m_Tree || !m_World)
        return;

    const std::vector<UI::Interaction::ItemId> sel = m_Selection->GetSelection();
    if (sel.empty())
    {
        m_Tree->SyncSelectionVisuals();
        return;
    }

    std::vector<UI::Interaction::ItemId> validIds;
    validIds.reserve(sel.size());
    for (auto itemId : sel)
    {
        const TreeId tid = static_cast<TreeId>(itemId);
        if (tid == 0 || tid == kRootId)
            continue;

        ECS::EntityHandle h = HierarchyDataProvider::Decode(tid);
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;

        validIds.push_back(itemId);
    }

    if (validIds.empty())
    {
        m_Selection->Clear();
        m_Tree->SyncSelectionVisuals();
        return;
    }

    UI::Interaction::ItemId anchor = m_Selection->GetAnchor();
    if (std::find(validIds.begin(), validIds.end(), anchor) == validIds.end())
        anchor = validIds.front();

    ApplySelectionProgrammatic(validIds, anchor);
}

void HierarchyPanel::ApplySortSettings()
{
    if (!m_Provider)
        return;
    m_Provider->SetSortMode(m_SortMode);
    m_Provider->SetSortDirection(m_SortDirection);
    Refresh();
}

// has-* follows whether a bar is really on screen, so the navigation row only
// rides up when there is a bar to make room for.
void HierarchyPanel::SetHorizontalBarPresent(bool present)
{
    if (m_Tree)
    {
        if (present)
            m_Tree->AddClass("has-horizontal-scroll");
        else
            m_Tree->RemoveClass("has-horizontal-scroll");
    }
    if (m_NavigationBar)
        m_NavigationBar->SetAboveHorizontalScrollbar(present);
}

Scene::SceneHierarchyUi HierarchyPanel::CaptureHierarchyUiForSceneSave()
{
    Scene::SceneHierarchyUi out{};
    if (!m_Tree || !m_World)
        return out;

    m_Tree->ForEachExpanded([&](TreeId id)
                            {
        if (id == 0 || id == kRootId)
            return;
        ECS::EntityHandle h = HierarchyDataProvider::Decode(id);
        if (!h.IsValid() || !m_World->IsValid(h))
            return;
        std::string t = GetEntitySceneTagString(*m_World, h);
        if (!t.empty())
            out.expandedSceneEntityTags.push_back(std::move(t)); });
    std::sort(out.expandedSceneEntityTags.begin(), out.expandedSceneEntityTags.end());

    if (!m_Selection)
        return out;

    const std::vector<UI::Interaction::ItemId> sel = m_Selection->GetSelection();
    const TreeId anchorTid = static_cast<TreeId>(m_Selection->GetAnchor());
    ECS::EntityHandle anchorHandle = HierarchyDataProvider::Decode(anchorTid);
    std::string anchorTag;
    if (anchorHandle.IsValid() && m_World->IsValid(anchorHandle))
        anchorTag = GetEntitySceneTagString(*m_World, anchorHandle);

    std::unordered_set<std::string> seen;
    if (!anchorTag.empty())
    {
        out.selectionSceneEntityTagsOrdered.push_back(anchorTag);
        seen.insert(anchorTag);
    }
    for (auto itemId : sel)
    {
        const TreeId tid = static_cast<TreeId>(itemId);
        if (tid == 0 || tid == kRootId)
            continue;
        ECS::EntityHandle h = HierarchyDataProvider::Decode(tid);
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;
        std::string t = GetEntitySceneTagString(*m_World, h);
        if (t.empty() || seen.count(t) != 0)
            continue;
        seen.insert(t);
        out.selectionSceneEntityTagsOrdered.push_back(std::move(t));
    }

    return out;
}

void HierarchyPanel::RestoreHierarchyUiFromSceneAfterSceneCommit()
{
    if (!m_Tree || !m_Provider || !m_World || !m_Selection)
        return;

    std::optional<Scene::SceneHierarchyUiFromFile> pending;
    if (m_PendingHierarchyUiProvider)
        pending = m_PendingHierarchyUiProvider();

    if (!pending.has_value())
        return;

    m_Tree->ClearExpansionState();

    if (pending->hadSection)
    {
        for (const std::string& tag : pending->ui.expandedSceneEntityTags)
        {
            if (tag.empty())
                continue;
            ECS::EntityHandle h = FindEntityBySceneTag(*m_World, tag);
            if (h.IsValid())
            {
                const TreeId tid = HierarchyDataProvider::Encode(h);
                if (tid != 0 && tid != kRootId && m_Provider->IsExpandable(tid))
                    m_Tree->SetExpanded(tid, true);
            }
        }
    }

    m_Tree->RefreshFromProvider();

    if (pending->hadSection)
    {
        const auto& ordered = pending->ui.selectionSceneEntityTagsOrdered;
        if (!ordered.empty())
        {
            std::vector<UI::Interaction::ItemId> ids;
            ids.reserve(ordered.size());
            for (const std::string& tag : ordered)
            {
                if (tag.empty())
                    continue;
                ECS::EntityHandle h = FindEntityBySceneTag(*m_World, tag);
                if (h.IsValid())
                {
                    const TreeId tid = HierarchyDataProvider::Encode(h);
                    if (tid != 0 && tid != kRootId)
                        ids.push_back(static_cast<UI::Interaction::ItemId>(tid));
                }
            }
            if (!ids.empty())
            {
                const UI::Interaction::ItemId anchor = ids.front();
                ApplySelectionProgrammatic(ids, anchor);
            }
            else
            {
                m_Selection->Clear();
                m_Tree->SyncSelectionVisuals();
            }
        }
        else
        {
            m_Selection->Clear();
            m_Tree->SyncSelectionVisuals();
        }
    }
    else
    {
        m_Selection->Clear();
        m_Tree->SyncSelectionVisuals();
    }
}

void HierarchyPanel::SetTreeRowHeight(float px)
{
    if (m_Tree)
    {
        m_Tree->SetRowHeight(px);
    }
}

float HierarchyPanel::GetTreeRowHeight() const
{
    return m_Tree ? m_Tree->GetRowHeight() : 20.0f;
}

void HierarchyPanel::SetTreeChildIndent(float px)
{
    if (m_Tree)
    {
        m_Tree->SetChildIndent(px);
    }
}

void HierarchyPanel::SetTreeIconSize(float px)
{
    m_LastTreeIconSizePx = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
    if (m_Tree)
    {
        ApplyTreeTitleIconLayoutVars(m_Tree, m_LastTreeIconSizePx);
        m_Tree->SetIconSize(m_LastTreeIconSizePx);
        m_Tree->RefreshFromProvider();
    }
    if (m_NavigationBar)
        m_NavigationBar->SetItemSize(m_LastTreeIconSizePx);
}

void HierarchyPanel::ApplyTreeIconSizeFromScroll(float scrollY)
{
    if (!m_OnTreeIconSizeWheelCommit)
        return;
    const float resized = EditorTreeIconSizeAfterResizeGesture(
        m_LastTreeIconSizePx, scrollY, kMaxEditorHierarchyTreeIconSizePx);
    if (std::fabs(resized - m_LastTreeIconSizePx) > 0.1f)
        m_OnTreeIconSizeWheelCommit(resized);
}

void HierarchyPanel::ClearSearchHighlights()
{
    for (UIElement* el : m_SearchHighlightedElements)
    {
        if (el)
            el->RemoveClass("search-match");
    }
    m_SearchHighlightedElements.clear();
    m_SearchMatchIds.clear();
    m_CurrentSearchText.clear();
}

void HierarchyPanel::ApplySearchFilter(const std::string& searchText)
{
    ClearSearchHighlights();
    m_SearchVisibleIds.clear();
    m_CurrentSearchText = searchText;
    if (searchText.empty() || !m_Provider || !m_Tree)
    {
        if (m_Tree)
            m_Tree->SetVisibilityFilter({});
        return;
    }

    const Editor::HierarchySearchQuery query = Editor::HierarchySearchQuery::Parse(searchText);
    if (query.Empty())
    {
        m_Tree->SetVisibilityFilter({});
        return;
    }

    // Resolve type names once per query. Component names are registered dynamically, so this
    // automatically covers engine, game, managed, and package components without a hardcoded list.
    struct SearchableComponentType
    {
        ECS::ComponentTypeId Id = 0;
        std::string FullName;
        std::string LeafName;
    };
    std::vector<SearchableComponentType> componentTypes;
    for (const std::string& registeredName : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const auto* info = ECS::ComponentRegistry::GetComponentInfo(registeredName);
        if (!info)
            continue;
        SearchableComponentType type;
        type.Id = info->TypeId;
        type.FullName = registeredName;
        std::transform(type.FullName.begin(), type.FullName.end(), type.FullName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t namespaceEnd = type.FullName.rfind("::");
        type.LeafName = namespaceEnd == std::string::npos ? type.FullName : type.FullName.substr(namespaceEnd + 2);
        componentTypes.push_back(std::move(type));
    }

    std::unordered_set<TreeId> ancestorsToExpand;
    std::function<bool(TreeId, const std::string&)> collectMatching = [&](TreeId id, const std::string& parentPath) -> bool
    {
        bool matches = false;
        std::string path = parentPath;
        if (id != 0 && id != kRootId)
        {
            const char* label = m_Provider->GetLabel(id);
            const std::string_view name = label ? std::string_view(label) : std::string_view{};
            path += "/";
            path += name;
            const std::string idText = std::to_string(id);
            const ECS::EntityHandle entity = HierarchyDataProvider::Decode(id);
            matches = query.Matches(
                {name, idText, path,
                 [this, entity, &componentTypes](std::string_view value, bool exact)
                 {
                     if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                         return false;
                     for (const SearchableComponentType& type : componentTypes)
                     {
                         const bool nameMatches = exact
                                                      ? (type.FullName == value || type.LeafName == value)
                                                      : (type.FullName.find(value) != std::string::npos ||
                                                         type.LeafName.find(value) != std::string::npos);
                         if (nameMatches && m_World->HasComponent(entity, type.Id))
                             return true;
                     }
                     return false;
                 }},
                m_SearchFieldScope);
            if (matches)
            {
                m_SearchMatchIds.insert(id);
            }
        }

        bool descendantMatches = false;
        int childCount = m_Provider->GetChildCount(id);
        for (int i = 0; i < childCount; ++i)
        {
            TreeId childId = m_Provider->GetChildId(id, i);
            if (childId == 0 || childId == kRootId)
                continue;
            if (collectMatching(childId, path))
                descendantMatches = true;
        }

        const bool visible = matches || descendantMatches;
        if (visible && id != 0 && id != kRootId)
            m_SearchVisibleIds.insert(id);
        if (descendantMatches && id != 0 && id != kRootId)
            ancestorsToExpand.insert(id);
        return visible;
    };
    int rootCount = m_Provider->GetRootCount();
    for (int r = 0; r < rootCount; ++r)
        collectMatching(m_Provider->GetRootId(r), {});

    for (TreeId id : ancestorsToExpand)
        m_Tree->SetExpanded(id, true);
    m_Tree->SetVisibilityFilter([this](TreeId id)
                                { return m_SearchVisibleIds.count(id) != 0; });
}

std::vector<UI::Interaction::ItemId> HierarchyPanel::GetSelectionItemIds() const
{
    if (!m_Selection)
        return {};
    return m_Selection->GetSelection();
}

UI::Interaction::ItemId HierarchyPanel::GetSelectionAnchor() const
{
    return m_Selection ? m_Selection->GetAnchor() : 0;
}

void HierarchyPanel::ApplySelectionProgrammatic(const std::vector<UI::Interaction::ItemId>& ids,
                                                UI::Interaction::ItemId anchor)
{
    if (!m_Selection || !m_Tree)
        return;

    // SyncSelectionVisuals fires the TreeView selection-changed callback which in turn
    // invokes m_OnSelectEntity/m_OnSelectEntities. When this programmatic apply is itself
    // driven by a SceneView marquee pick, re-entering those callbacks bounces back into
    // HierarchyPanel::SelectEntities and recurses forever. Guard with m_SkipOnSelectEntity
    // the same way SyncSelectionWithSceneViewPick does.
    struct SkipGuard
    {
        bool& flag;
        explicit SkipGuard(bool& f)
            : flag(f) { flag = true; }
        ~SkipGuard() { flag = false; }
    } guard{m_SkipOnSelectEntity};

    m_SuppressSelectionUndo = true;
    m_Selection->SetSelection(ids, anchor);
    m_LastSelectionIds = m_Selection->GetSelection();
    m_LastSelectionAnchor = m_Selection->GetAnchor();
    m_SuppressSelectionUndo = false;

    m_Tree->RefreshFromProvider();
    m_Tree->SyncSelectionVisuals();
}

void HierarchyPanel::SyncSelectionWithSceneViewPick(ECS::EntityHandle entity)
{
    if (!m_Selection || !m_Tree || !m_World || !m_Provider)
        return;

    // When the hierarchy itself initiated the selection (e.g. Cmd/Shift-click), don't
    // let the SceneView round-trip reset the multi-selection back to a single item.
    if (m_SkipSyncFromSceneView)
        return;

    // Mirror TreeView::RefreshFromProvider: applying selection during drag/drop or nested input
    // routing can re-enter TreeView and corrupt state. Defer to the next safe UI tick.
    if (UIElement::IsInEventDispatch())
    {
        const ECS::EntityHandle captured = entity;
        const bool skipSync = m_SkipSyncFromSceneView;
        this->PostSafeAction([this, captured, skipSync]()
                             {
                                 if (skipSync) return;
                                 SyncSelectionWithSceneViewPick(captured); });
        return;
    }

    struct SkipGuard
    {
        bool& flag;
        explicit SkipGuard(bool& f)
            : flag(f)
        {
            flag = true;
        }
        ~SkipGuard() { flag = false; }
    } guard{m_SkipOnSelectEntity};

    if (!entity.IsValid())
    {
        m_Selection->Clear();
    }
    else if (!m_World->IsValid(entity))
    {
        return;
    }
    else
    {
        const TreeId tid = HierarchyDataProvider::Encode(entity);
        if (tid == 0 || tid == kRootId)
            return;

        ExpandAncestors(entity);
        m_Selection->SetSingle(static_cast<UI::Interaction::ItemId>(tid));
    }

    m_Tree->RefreshFromProvider();
    m_Tree->SyncSelectionVisuals();

    // Scroll the picked row into view (after RefreshFromProvider so the
    // expanded ancestors are part of the flattened row list).
    if (entity.IsValid())
    {
        const TreeId tid = HierarchyDataProvider::Encode(entity);
        if (tid != 0 && tid != kRootId)
            m_Tree->ScrollToItem(tid);
    }
}

// Expand every ancestor of `entity` so its row is present in the flattened
// tree. Callers must RefreshFromProvider afterwards.
void HierarchyPanel::ExpandAncestors(ECS::EntityHandle entity)
{
    ECS::EntityHandle cur = entity;
    while (cur.IsValid() && m_World->IsValid(cur))
    {
        auto* p = m_World->GetComponent<Parent>(cur);
        if (!p || !p->parent.IsValid() || !m_World->IsValid(p->parent))
            break;
        const TreeId parentTid = HierarchyDataProvider::Encode(p->parent);
        if (parentTid != 0 && parentTid != kRootId)
            m_Tree->SetExpanded(parentTid, true);
        cur = p->parent;
    }
}

void HierarchyPanel::SelectEntities(const std::vector<ECS::EntityHandle>& entities)
{
    if (!m_Provider || !m_Tree || !m_World || !m_Selection || entities.empty())
        return;

    // A hierarchy click is forwarded to the Scene View so its gizmos and transform tool stay
    // synchronized. The Scene View reports the resulting full selection back through this method;
    // do not treat that round-trip as a Scene View pick and reveal/scroll the row in its source tree.
    if (m_SkipSyncFromSceneView)
        return;

    std::vector<UI::Interaction::ItemId> ids;
    ids.reserve(entities.size());
    for (ECS::EntityHandle h : entities)
    {
        if (!h.IsValid() || !m_World->IsValid(h))
            continue;

        const TreeId tid = HierarchyDataProvider::Encode(h);
        if (tid == 0 || tid == kRootId)
            continue;

        ids.push_back(static_cast<UI::Interaction::ItemId>(tid));
    }

    if (ids.empty())
        return;

    // Reveal the most recent valid pick (the scene view's primary is the last
    // entry): expand its ancestors before the refresh, scroll after.
    ECS::EntityHandle reveal{};
    for (auto it = entities.rbegin(); it != entities.rend(); ++it)
    {
        if (it->IsValid() && m_World->IsValid(*it))
        {
            reveal = *it;
            break;
        }
    }
    if (reveal.IsValid())
        ExpandAncestors(reveal);

    const UI::Interaction::ItemId anchor = ids[0];
    ApplySelectionProgrammatic(ids, anchor);

    if (reveal.IsValid())
        m_Tree->ScrollToItem(HierarchyDataProvider::Encode(reveal));
}

void HierarchyPanel::SelectEntity(ECS::EntityHandle entity)
{
    if (!m_Provider || !m_Tree || !m_World || !m_World->IsValid(entity))
        return;

    // Find the TreeId for this entity by traversing the tree
    TreeId targetTid = HierarchyDataProvider::Encode(entity);

    // Helper lambda to recursively search for the entity
    std::function<bool(TreeId)> searchTree = [&](TreeId tid) -> bool
    {
        if (tid == targetTid)
        {
            // Found it - select this entity
            if (m_Selection)
            {
                m_Selection->SetSingle(tid);
            }
            m_Tree->RefreshFromProvider();

            // Also trigger selection callback
            if (m_OnSelectEntity)
            {
                m_OnSelectEntity(entity);
            }
            return true;
        }

        // Search children
        int childCount = m_Provider->GetChildCount(tid);
        for (int i = 0; i < childCount; ++i)
        {
            TreeId childId = m_Provider->GetChildId(tid, i);
            if (searchTree(childId))
                return true;
        }
        return false;
    };

    // Search from all roots
    int rootCount = m_Provider->GetRootCount();
    for (int i = 0; i < rootCount; ++i)
    {
        TreeId rootId = m_Provider->GetRootId(i);
        if (searchTree(rootId))
            break;
    }
}

} // namespace GameEngine
