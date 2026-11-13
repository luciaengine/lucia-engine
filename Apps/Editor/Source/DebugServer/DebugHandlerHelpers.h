#pragma once
#include "DebugServer/DebugHandlers.h"

#include "Automation/UiReplayCommandIds.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/LensFlareImporter.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "AssetCore/GUID.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/RenderLayer.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneIOContext.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "Core/DebugMetrics.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Types/Base64.h"
#include "DebugServer/AssistantSessionHandlers.h"
#include "DebugServer/BenchSceneHandlers.h"
#include "DebugServer/CameraProjectionReport.h"
#include "Editor/CaptureOutputDirectory.h"
#include "DebugServer/Components/ComponentValueReader.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/ElementPointerTarget.h"
#include "DebugServer/GpuToolingHandlers.h"
#include "DebugServer/GroundQueryDebugHandlers.h"
#include "DebugServer/MarkupDebugHandlers.h"
#include "DebugServer/PerformDropRequest.h"
#include "DebugServer/SkyDebugHandlers.h"
#include "DebugServer/ThumbnailDebugHandlers.h"
#include "DebugServer/InjectedInput.h"
#include "DebugServer/OpenAssetPath.h"
#include "DebugServer/Parallax/ParallaxDebugHandlers.h"
#include "DebugServer/Physics/PhysicsDebugHandlers.h"
#include "DebugServer/PlayModeRequest.h"
#include "DebugServer/PointerCoordsRequest.h"
#include "DebugServer/ReflectedEnumValue.h"
#include "DebugServer/SceneDegradedReport.h"
#include "DebugServer/TileMapDebugHandlers.h"
#include "DebugServer/SceneHierarchyReport.h"
#include "DebugServer/Components/SchemaComponentApply.h"
#include "DebugServer/Capture/RenderDocCapture.h"
#include "DebugServer/RenderGraphResourceFilter.h"
#include "DebugServer/ScreenshotCaptureWait.h"
#include "DebugServer/Terrain/TerrainDebugHandlers.h"
#include "DebugServer/UITreeDump.h"
#include "DebugServer/ViewFocusRequest.h"
#include "DebugServer/DockPanelRequest.h"
#include "DebugServer/Capture/WindowPrintCapture.h"
#include "UI/Interaction/DragDrop/DragDropManager.h"
#include "ECS/ComponentFactory.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ReflectionJson.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Settings/Render/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Assets/ShaderGlslOpen.h"
#include "EditorApplication/EditorApplication.h"
#include "Search/UniversalSearchController.h"
#include "EditorChangeNotifications.h"
#include "GameView/GameViewController.h"
#include "MovieRecorderController.h"
#include "Video/VideoWriter.h"
#include "EditorContextMenu/InterceptableContextMenu.h"
#include "Platform/Display.h"
#include "EditorPanelIds.h"
#include "EditorPanelManager.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/LogLevel.h"
#include "Logger/RingBufferSink.h"
#include "Panels/Animation/AnimationWindowPanel.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/AssetsPanel.h"
#include "Panels/BuildPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/GraphPanel.h"
#include "Graph/GraphKindChrome.h"
#include "Graph/GraphPanelFactory.h"
#include "Panels/SettingsPanel.h"
#include "SceneView/SceneViewPanel.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PlayMode/PlayModeManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Scene/SceneEditorController.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneView/SceneViewRenderCoordinator.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SceneViewController.h"
#include "Terrain/TerrainBrushTool.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainEntityProvisioning.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainGrass/TerrainGrassClamps.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "UI/Controls/DockspaceElement.h"
#include "Types/FlatMap.h"
#include "Types/Fnv1a.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TableView.h"
#include "UI/Controls/TextField.h"
#include "UI/ResolvedStyle.h"
#include "UI/UiContext.h"
#include "UI/Utf8Helpers.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/Spline/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[maybe_unused]] static bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Write one JSON value into a component's raw bytes for a single reflected field,
// dispatching on its FieldTypeId. Type-guarded so a mismatched JSON value is
// ignored rather than throwing. Float/Vec/Color accept a JSON array for the
// element-wise components (e.g. Light.Color = [r,g,b]).
// Returns false whenever the field was not written and the caller must say so: an
// AssetGuid/enum string that did not resolve (unknown asset path / unknown enumerator),
// an entity id naming no live entity, or a field type this writer has no representation
// for (Mat4/Bytes/Unknown). Reporting success for a write that changed nothing is what
// this contract exists to prevent. `world` supplies entity-handle liveness.
[[maybe_unused]] static bool WriteFieldFromJson(ECS::World& world, std::vector<uint8_t>& bytes,
                               const ECS::FieldInfo& f, const json& v)
{
    // The integer/enum stores below memcpy a value's low bytes into the field; that assumes a
    // little-endian host (every shipping target). A big-endian port would byte-swap.
    static_assert(std::endian::native == std::endian::little, "WriteFieldFromJson assumes little-endian");
    using ECS::FieldTypeId;
    const std::size_t off = f.Offset;
    const auto fits = [&](std::size_t n)
    { return off + n <= bytes.size(); };
    const auto writeFloats = [&](std::uint32_t count)
    {
        if (!v.is_array())
            return;
        for (std::uint32_t i = 0; i < count && i < v.size(); ++i)
        {
            if (!v[i].is_number() || !fits(i * 4 + 4))
                continue;
            float x = v[i].get<float>();
            std::memcpy(bytes.data() + off + i * 4, &x, 4);
        }
    };
    const auto storeInt = [&]<class T>()
    {
        if (v.is_number() && fits(sizeof(T)))
        {
            auto x = static_cast<T>(v.get<long long>());
            std::memcpy(bytes.data() + off, &x, sizeof(T));
        }
    };
    // Reflected enum fields accept only declared names or integer values.
    if (!f.EnumNames.empty() && f.Size <= sizeof(std::int64_t) && fits(f.Size))
    {
        const std::optional<std::int64_t> value = Editor::ResolveEnumFieldValue(f, v);
        if (!value)
            return false;
        std::memcpy(bytes.data() + off, &*value, f.Size);
        return true;
    }
    switch (f.Type)
    {
    case FieldTypeId::Bool:
        if (fits(1))
            bytes[off] = (v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0.0)) ? 1 : 0;
        break;
    case FieldTypeId::Int8:
        storeInt.operator()<std::int8_t>();
        break;
    case FieldTypeId::UInt8:
        storeInt.operator()<std::uint8_t>();
        break;
    case FieldTypeId::Int16:
        storeInt.operator()<std::int16_t>();
        break;
    case FieldTypeId::UInt16:
        storeInt.operator()<std::uint16_t>();
        break;
    case FieldTypeId::Int32:
        storeInt.operator()<std::int32_t>();
        break;
    case FieldTypeId::UInt32:
        storeInt.operator()<std::uint32_t>();
        break;
    case FieldTypeId::Int64:
        storeInt.operator()<std::int64_t>();
        break;
    case FieldTypeId::UInt64:
        storeInt.operator()<std::uint64_t>();
        break;
    case FieldTypeId::Double:
        if (v.is_number() && fits(8))
        {
            double x = v.get<double>();
            std::memcpy(bytes.data() + off, &x, 8);
        }
        break;
    case FieldTypeId::Float:
        if (v.is_array())
            writeFloats(f.Size / 4); // float[N] (e.g. Color[3])
        else if (v.is_number() && fits(4))
        {
            float x = v.get<float>();
            std::memcpy(bytes.data() + off, &x, 4);
        }
        break;
    case FieldTypeId::Vec2:
        writeFloats(2);
        break;
    case FieldTypeId::Vec3:
        writeFloats(3);
        break;
    case FieldTypeId::Vec4:
    case FieldTypeId::Quat:
    case FieldTypeId::Color:
        writeFloats(4);
        break;
    case FieldTypeId::String:
        if (v.is_string() && f.Size > 0 && fits(f.Size))
        {
            const std::string s = v.get<std::string>();
            const std::size_t n = std::min<std::size_t>(s.size(), f.Size - 1);
            std::memcpy(bytes.data() + off, s.data(), n);
            bytes[off + n] = 0;
        }
        break;
    case FieldTypeId::AssetGuid:
    {
        // An AssetRef<T> (MaterialRef/ModelRef/TextureRef) is a 16-byte GUID. Accept a GUID
        // string or {"guid":"..."}; the component write-back re-resolves it during extraction
        // (e.g. the renderer's MaterialRegistry::Find), so set_component can assign a material.
        const json* g = v.is_string()                           ? &v
                        : (v.is_object() && v.contains("guid"))  ? &v.at("guid")
                                                                 : nullptr;
        if (!g || !g->is_string())
            return v.is_null(); // null = leave the ref as-is (ok); any other non-string is malformed -> report
        if (!fits(GUID::kSize))
            break;
        const std::string s = g->get<std::string>();
        if (s.empty())
        {
            // "" CLEARS the reference. It is what a read emits for an unset ref, so this is the
            // half that makes get_entity_components output writable back: a payload carrying an
            // unset ref has to land as unset, not silently keep whatever the target already held.
            // (JSON null, above, means "no value supplied" and still leaves the ref alone; the
            // read shape never emits it.)
            std::memset(bytes.data() + off, 0, GUID::kSize);
            return true;
        }
        // The GUID string ctor fills zeros (not throw) on a malformed string. If it isn't a
        // raw GUID, treat the string as an asset PATH and resolve it (so MCP can assign any
        // AssetRef<> by path, e.g. "UI/Hud.uxml", matching the scene format).
        GUID guid{s};
        if (guid.IsNull())
            guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(std::filesystem::path(s));
        if (!guid.IsNull())
        {
            std::memcpy(bytes.data() + off, guid.GetData().data(), GUID::kSize);
            return true;
        }
        // Non-empty string that's neither a raw GUID nor a resolvable asset path: leave the
        // existing ref untouched and report it (restores the diagnostic the deleted UIDocument
        // special-case had) instead of a silent no-op masquerading as success.
        return false;
    }
    case FieldTypeId::EntityHandle:
    {
        if (!fits(sizeof(std::uint32_t)))
            return false;
        // JSON null clears the link. That IS a write, unlike the zero below.
        if (v.is_null())
        {
            const ECS::EntityHandle cleared{};
            std::memcpy(bytes.data() + off, &cleared.id, sizeof cleared.id);
            return true;
        }
        if (!v.is_number())
            return false;
        const auto raw = v.get<std::int64_t>();
        if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max())
            return false;
        // EntityHandle::IsValid only tests the kInvalidEntity sentinel, so a ZEROED id
        // passes it while naming no entity — versions are minted from 1, so (0,0) never
        // exists. Liveness lives in the World; ask it, or a raw byte round-trip would
        // store a handle that every reader then silently ignores.
        const ECS::EntityHandle handle{static_cast<std::uint32_t>(raw)};
        if (!world.IsValid(handle))
            return false;
        std::memcpy(bytes.data() + off, &handle.id, sizeof handle.id);
        return true;
    }
    case FieldTypeId::Mat4:
    case FieldTypeId::Bytes:
    case FieldTypeId::Unknown:
    default:
        return false; // no JSON representation here — report rather than claim a write
    }
    return true; // written, or an acceptable type-mismatch no-op
}

// Set fields on a reflected component from a JSON object, keyed by field name
// (exact or case-insensitive — set_component historically used camelCase keys
// like "fovY" for the PascalCase field FovY). Adds the component (default-
// constructed via the factory) if missing. Returns false if the type has no
// field table.
[[maybe_unused]] static bool SetReflectedComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                        ECS::ComponentTypeId typeId, const json& values,
                                        std::vector<std::string>* outUnapplied = nullptr)
{
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (fields.empty())
        return false;

    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
    {
        if (!ECS::ComponentFactory::Create(*world, entity, typeId))
            return false;
        if (!world->CaptureComponentBytes(entity, typeId, bytes))
            return false;
    }

    if (values.is_object())
    {
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            bool matched = false;
            for (const ECS::FieldInfo& f : fields)
            {
                if (f.Name == it.key() || IEquals(f.Name, it.key()))
                {
                    matched = true;
                    // Honor field policy: don't let set_component write fields a
                    // component marked internal (Hidden) or non-editable (ReadOnly).
                    if (ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden | ECS::FieldFlags::ReadOnly))
                    {
                        if (outUnapplied)
                            outUnapplied->push_back(it.key()); // read-only/hidden: not settable here
                    }
                    else if (!WriteFieldFromJson(*world, bytes, f, it.value()) && outUnapplied)
                    {
                        if (!f.EnumNames.empty())
                            outUnapplied->push_back(it.key() + " (valid: " + Editor::DescribeEnumFieldValues(f) + ")");
                        else
                            outUnapplied->push_back(it.key());
                    }
                    break;
                }
            }
            if (!matched && outUnapplied)
                outUnapplied->push_back(it.key()); // no field by that name on this component
        }
    }
    return world->ApplyComponentBytesImmediate(entity, typeId, bytes);
}

// Apply named field values from JSON onto a component attached to an entity.
// Adds the component if it does not already exist. Returns a RefuseRequest
// refusal on failure or a null json (is_null()) on success. Callers should check
// result.is_null() to distinguish success from error.
// Result of ApplyComponentValues. Error is null on success (the plain-json
// contract error returns already use; the implicit constructor keeps them
// terse). A branch that applies a data-only fast path sets SuppressNotify so
// set_component doesn't announce the write — the policy lives next to the
// branch that implements it. Defaulting to notify fails safe: a forgotten
// flag costs a redundant inspector rebuild, never silent UI staleness.
struct ComponentApplyResult
{
    json Error;
    bool SuppressNotify = false;

    ComponentApplyResult(json error = {}) : Error(std::move(error)) {}
};

// The one message a component write returns for keys that named no field. Shared by the
// reflected path, the hand-written branches and the create_entity terrain payload, so a
// caller cannot tell from the wording which internal route their component took.
[[maybe_unused]] static json UnappliedFieldsError(std::string_view operation, const std::string& componentName,
                                 const std::vector<std::string>& unapplied)
{
    std::string joined;
    for (std::size_t i = 0; i < unapplied.size(); ++i)
        joined += (i ? ", " : "") + unapplied[i];
    return Editor::RefuseRequest(std::string(operation) + " " + componentName +
                                     ": fields not applied (unknown name, read-only, "
                                     "unresolved asset/enum, dead entity id, or a field "
                                     "type this path cannot express): " +
                                     joined);
}

// The replacement for a retired pre-volume modifier name, or null when the name is
// not one. The five names are no longer components — only a scene FORMAT the
// migration schemas still parse — but those schemas stay registered, so a retired
// name reaching the generic schema fallback would FIND one: the lone-ApplyProperty
// path would park a half-parsed block on the entity and answer ok, and a later save
// would re-emit a legacy block that the next load turns back into a live modifier.
// Silent at the call, materialised a save later. Refused instead, naming the
// replacement (terrain-editing-ux-design 1.2).
//
// Only this surface refuses. The other schema callers — the preserved-field
// resolver and the three VCS revert sites — REPLAY already-authored data, which
// is exactly what the migration exists to consume. set_component is new authoring.
//
// Matched case-INSENSITIVELY, because every name resolver this sits in front of
// is: SceneSchemaRegistry::Find lowercases through ToKey, and
// ComponentFieldRegistry::FindByName documents IEquals matching for exactly this
// caller (user-typed MCP names). An exact compare here would refuse
// "TerrainFlattenModifier" and wave "terrainflattenmodifier" straight through to
// the schema it still resolves to.
[[maybe_unused]] static const char* RetiredModifierReplacement(const std::string& componentName)
{
    if (IEquals(componentName, "TerrainFlattenModifier"))
        return "TerrainModifierVolume + TerrainFlattenEffect";
    if (IEquals(componentName, "TerrainNoiseModifier"))
        return "TerrainModifierVolume + TerrainNoiseEffect";
    if (IEquals(componentName, "TerrainStampModifier"))
        return "TerrainModifierVolume + TerrainStampEffect";
    if (IEquals(componentName, "TerrainPaintLayerModifier"))
        return "TerrainModifierVolume + TerrainPaintLayerEffect";
    if (IEquals(componentName, "TerrainSplineModifier"))
        return "TerrainModifierVolume (Shape = SplineArea) + TerrainFlattenEffect";
    return nullptr;
}

// Range invariants the reflected writer cannot know about. SetReflectedComponentFields lands raw
// bytes with no bounds checking, so a component whose fields carry ranges runs its own clamp pass
// here — the SAME pass its scene schema runs after parsing a property, so a value set over IPC and
// the same value loaded from a .scene land identically. Whole-component and idempotent, so it needs
// no knowledge of which fields the write touched.
[[maybe_unused]] static void ApplyReflectedComponentInvariants(ECS::World* world, ECS::EntityHandle entity,
                                              ECS::ComponentTypeId typeId)
{
    if (typeId == ECS::GetComponentTypeId<Components::TerrainGrass>())
    {
        if (auto* grass = world->GetComponentForWrite<Components::TerrainGrass>(entity))
            TerrainGrass::ClampFields(*grass);
    }
}

// PhysicsBody.motionType from its enumerator name or integer index; nullopt for anything else,
// so a typo is refused instead of landing as Dynamic.
[[maybe_unused]] static std::optional<Physics::MotionType> ParseMotionType(const json& value)
{
    if (value.is_string())
    {
        const std::string& name = value.get_ref<const std::string&>();
        if (name == "Static")
            return Physics::MotionType::Static;
        if (name == "Kinematic")
            return Physics::MotionType::Kinematic;
        if (name == "Dynamic")
            return Physics::MotionType::Dynamic;
        return std::nullopt;
    }
    if (value.is_number_integer())
    {
        const int64_t index = value.get<int64_t>();
        if (index >= static_cast<int64_t>(Physics::MotionType::Static) &&
            index <= static_cast<int64_t>(Physics::MotionType::Dynamic))
            return static_cast<Physics::MotionType>(index);
    }
    return std::nullopt;
}

[[maybe_unused]] static ComponentApplyResult ApplyComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    // Every hand-written branch below understands a fixed set of field names, so it reads
    // through the tracking reader and the tail reports whatever it never looked at. Without
    // that, a misspelled or wrong-case key is indistinguishable from an absent one and
    // set_component answers `ok: true` for a write that changed nothing.
    Editor::ComponentValueReader fields(values);
    bool suppressNotify = false;

    if (componentName == "Transform")
    {
        // Merge onto the existing transform: a partial write ({position} only)
        // must not reset rotation/scale to defaults — a scale reset silently
        // collapses scale-derived volumes (reflection-probe influence boxes).
        Mathematics::Vector3 pos{};
        Mathematics::Quaternion rot{};
        Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
        if (const auto* existing = world->GetComponent<Components::Transform>(entity))
        {
            pos = existing->GetPosition();
            rot = existing->GetRotation();
            scl = existing->GetScale();
        }
        if (fields.Has("position"))
        {
            auto& p = fields["position"];
            pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
        }
        if (fields.Has("rotation"))
        {
            auto& r = fields["rotation"];
            rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
        }
        if (fields.Has("scale"))
        {
            auto& s = fields["scale"];
            scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
        }
        auto xf = Components::Transform::FromTRS(pos, rot, scl);
        if (!world->GetComponent<Components::Transform>(entity))
            world->AddComponentImmediate(entity, xf);
        else
            *world->GetComponentForWrite<Components::Transform>(entity) = xf;
    }
    else if (componentName == "Parent")
    {
        // No reflected field table (EntityHandle field, [DoNotSerialize]) — apply directly.
        // Deliberately emits no EditorChangeNotifications: this models a gameplay/script
        // reparent (the ECS data-only set fast path), which the Hierarchy panel picks up
        // via its Changed<Parent> scan, not the editor command layer. parent=0 un-parents
        // (stored as the invalid-handle sentinel, matching ReparentCommand's Parent{}).
        const uint32_t parentId = fields.Value("parent", 0u);
        const ECS::EntityHandle parentHandle =
            parentId != 0 ? ECS::EntityHandle(parentId) : ECS::EntityHandle::Invalid();
        if (parentId != 0 && !world->IsValid(parentHandle))
            return Editor::RefuseRequest("Invalid parent entity");
        if (parentHandle == entity)
            return Editor::RefuseRequest("Cannot parent an entity to itself");
        world->AddComponentImmediate(entity, Components::Parent{parentHandle});
        suppressNotify = true;
    }
    else if (componentName == "SplineComponent")
    {
        auto* splineService = SplineECS::SplineService::TryGet();
        if (!splineService)
            return Editor::RefuseRequest("SplineService not available");

        bool closed = fields.Value("closed", false);
        float defaultRadius = fields.Value("DefaultRadius", 5.0f);
        uint32_t typeVal = fields.Value("type", 0u);
        auto splineType = static_cast<Spline::SplineType>(typeVal);

        // The entity's own spline is rewritten in place, keeping its handle, so set_component's
        // spline snapshot can put the old points back on undo; a first set creates one.
        auto* existing = world->GetComponent<Components::SplineComponent>(entity);
        const bool reuse = existing && splineService->IsValid(SplineECS::SplineHandle(existing->SplineDataIndex,
                                                                                      existing->SplineDataGeneration));
        const SplineECS::SplineHandle handle =
            reuse ? SplineECS::SplineHandle(existing->SplineDataIndex, existing->SplineDataGeneration)
                  : splineService->CreateSpline(splineType, closed);
        auto* data = splineService->GetSplineData(handle);
        if (!data)
            return Editor::RefuseRequest("Failed to create spline data");
        data->Type = splineType;
        data->Closed = closed;
        data->Points.clear();

        if (fields.Has("points") && fields["points"].is_array())
        {
            for (const auto& pt : fields["points"])
            {
                float x = pt.value("x", 0.0f);
                float y = pt.value("y", 0.0f);
                float z = pt.value("z", 0.0f);
                float r = pt.value("radius", defaultRadius);
                data->AddPoint(Mathematics::Vector3(x, y, z), r);
            }
        }
        data->MarkDirty();
        Spline::RebuildSplineCache(*data);

        Components::SplineComponent sc{};
        sc.SplineDataIndex = handle.Index();
        sc.SplineDataGeneration = handle.Generation();
        sc.DefaultRadius = defaultRadius;

        if (!existing)
            world->AddComponentImmediate(entity, sc);
        else
            *world->GetComponentForWrite<Components::SplineComponent>(entity) = sc;
    }
    else if (componentName == "ParticleCollisionEventsBuffer")
    {
        if (!world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
            world->AddComponentImmediate(entity, Components::ParticleCollisionEventsBuffer{});
        if (auto* c = world->GetComponentForWrite<Components::ParticleCollisionEventsBuffer>(entity))
            c->Clear();
    }
    else if (componentName == "PhysicsBody")
    {
        // Physics components live under PhysicsECS/Include/PhysicsECS/Components,
        // outside the component scanner's reflected glob, so they need explicit
        // handlers here (the reflected fallback below can't find them).
        // motionType is validated before anything is written, so a refused value
        // leaves the entity untouched.
        std::optional<Physics::MotionType> motionType;
        if (fields.Has("motionType"))
        {
            motionType = ParseMotionType(fields["motionType"]);
            if (!motionType)
                return Editor::RefuseRequest("PhysicsBody.motionType must be \"Static\", \"Kinematic\", "
                                             "\"Dynamic\" or 0/1/2; got " + fields["motionType"].dump());
        }
        if (!world->HasComponent<Components::PhysicsBody>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsBody{});
        auto* c = world->GetComponentForWrite<Components::PhysicsBody>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsBody");
        if (fields.Has("mass"))
            c->mass = fields["mass"].get<float>();
        if (fields.Has("linearDamping"))
            c->linearDamping = fields["linearDamping"].get<float>();
        if (fields.Has("angularDamping"))
            c->angularDamping = fields["angularDamping"].get<float>();
        if (fields.Has("gravityScale"))
            c->gravityScale = fields["gravityScale"].get<float>();
        if (fields.Has("centerOfMassOffset") && fields["centerOfMassOffset"].is_object())
        {
            const auto& o = fields["centerOfMassOffset"];
            c->centerOfMassOffsetX = o.value("x", c->centerOfMassOffsetX);
            c->centerOfMassOffsetY = o.value("y", c->centerOfMassOffsetY);
            c->centerOfMassOffsetZ = o.value("z", c->centerOfMassOffsetZ);
        }
        if (fields.Has("centerOfMassOffsetX"))
            c->centerOfMassOffsetX = fields["centerOfMassOffsetX"].get<float>();
        if (fields.Has("centerOfMassOffsetY"))
            c->centerOfMassOffsetY = fields["centerOfMassOffsetY"].get<float>();
        if (fields.Has("centerOfMassOffsetZ"))
            c->centerOfMassOffsetZ = fields["centerOfMassOffsetZ"].get<float>();
        if (motionType)
            c->motionType = *motionType;
        c->initialized = false;
    }
    else if (componentName == "PhysicsCollider")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        auto* c = world->GetComponentForWrite<Components::PhysicsCollider>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsCollider");
        if (fields.Has("isTrigger"))
            c->isTrigger = fields["isTrigger"].get<bool>();
    }
    else if (componentName == "BoxColliderShape")
    {
        // A collider shape implies a PhysicsCollider (required component).
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::BoxColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::BoxColliderShape{});
        auto* c = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add BoxColliderShape");
        if (fields.Has("halfExtentsX"))
            c->halfExtentsX = fields["halfExtentsX"].get<float>();
        if (fields.Has("halfExtentsY"))
            c->halfExtentsY = fields["halfExtentsY"].get<float>();
        if (fields.Has("halfExtentsZ"))
            c->halfExtentsZ = fields["halfExtentsZ"].get<float>();
    }
    else if (componentName == "SphereColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::SphereColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::SphereColliderShape{});
        auto* c = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add SphereColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
    }
    else if (componentName == "CapsuleColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::CapsuleColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::CapsuleColliderShape{});
        auto* c = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add CapsuleColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
        if (fields.Has("halfHeight"))
            c->halfHeight = fields["halfHeight"].get<float>();
        if (fields.Has("axis"))
            c->axis = static_cast<uint8>(fields["axis"].get<int>());
    }
    else
    {
        // UIDocument, TerrainGrass and every other reflected component (incl. AssetRef<> fields
        // by GUID or path and enum fields by name) flow through the generic reflection path
        // below — no per-component special-case needed since WriteFieldFromJson learned
        // path-resolve + enum-by-name. A hand-written branch here would shadow the reflected
        // field table and silently cover fewer fields than the component has.
        ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(componentName);
        std::vector<std::string> unapplied;
        if (typeId != 0 && SetReflectedComponentFields(world, entity, typeId, values, &unapplied))
        {
            // Before reporting anything: the bytes are already in the world, so a partially
            // applied write must not leave out-of-range values behind on the error path.
            ApplyReflectedComponentInvariants(world, entity, typeId);
            // The component applied, but report any keys that didn't so MCP/LLM callers
            // don't read a silent no-op as success.
            if (!unapplied.empty())
                return UnappliedFieldsError("set_component", componentName, unapplied);
            return json{};
        }
        // Schema-only components (Terrain, ...) have no reflected field table; drive them
        // through the same hand-written schema the .scene loader uses. Reflected components
        // already returned above — PostProcessVolume, SkyEnvironment and VolumetricFogEffect
        // are all reflected, so their schema entries are unreachable from here.
        if (const char* replacement = RetiredModifierReplacement(componentName))
            return Editor::RefuseRequest(componentName + " is retired; set " + replacement +
                                             " instead");
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(componentName))
        {
            const std::string refusal = Editor::ApplyComponentViaSchema(*world, entity, componentName, *schema, values);
            return refusal.empty() ? json{} : Editor::RefuseRequest(refusal);
        }
        return Editor::RefuseRequest("Unknown component: " + componentName);
    }

    // A hand-written branch ran. Anything it never asked about wrote nothing, so report it
    // in the same words the reflected path uses rather than answering `ok` for a no-op.
    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("set_component", componentName, unconsumed);

    ComponentApplyResult applied;
    applied.SuppressNotify = suppressNotify;
    return applied;
}

// Resolve an IPC component name to its ECS type id: reflected components via
// the field registry, schema-only ones (Terrain, PostProcessVolume, ...) via
// the ECS component-name registry (suffix match tolerates namespaced names).
[[maybe_unused]] static ECS::ComponentTypeId ResolveComponentTypeIdByName(const std::string& componentName)
{
    if (const ECS::ComponentTypeId reflected = ECS::ComponentFieldRegistry::FindByName(componentName);
        reflected != 0)
        return reflected;
    const std::string suffix = "::" + componentName;
    for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const bool match = (regName == componentName) ||
                           (regName.size() >= suffix.size() &&
                            regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
        if (match)
            return ECS::ComponentRegistry::GetComponentTypeId(regName);
    }
    return 0;
}

// A component's on/off state is its ECS::ComponentDisabled tag, not a field, but a write
// still names it "enabled" (or "Enabled"). The key is taken out of what the field writers see
// and applied as the tag once the rest of the write landed — for a type that switches through
// the tag (ComponentRegistry::SwitchesThroughDisabledTag). Any other type's key goes to the
// field writers: a post-process effect's or a terrain effect's own Enabled field, or, for a type
// declared NotToggleable, a field it does not have.
[[maybe_unused]] static ComponentApplyResult ApplyComponentValues(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    const ECS::ComponentTypeId typeId = ResolveComponentTypeIdByName(componentName);
    if (!values.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return ApplyComponentFields(world, entity, componentName, values);

    json fieldValues = values;
    std::optional<bool> enabled;
    for (const char* key : {"enabled", "Enabled"})
    {
        if (!fieldValues.contains(key))
            continue;
        if (!fieldValues[key].is_boolean())
            return Editor::RefuseRequest(componentName + "." + key + " must be true or false");
        enabled = fieldValues[key].get<bool>();
        fieldValues.erase(key);
    }

    ComponentApplyResult applied = ApplyComponentFields(world, entity, componentName, fieldValues);
    if (applied.Error.is_null() && enabled)
        world->SetComponentEnabledImmediate(entity, typeId, *enabled);
    return applied;
}

[[maybe_unused]] static json CreateDefaultTerrainForEntity(ECS::World* world, ECS::EntityHandle entity, const json& values)
{
    if (!world || !entity.IsValid())
        return Editor::RefuseRequest("Invalid terrain entity");

    // Same key-consumption contract set_component applies: a misspelled key here would
    // silently hand back a DEFAULT terrain (a planar one for a caller who asked for a
    // planet), which is exactly the kind of no-op that reads as success.
    Editor::ComponentValueReader fields(values);

    Components::Terrain terrainComp{};
    terrainComp.SizeX = fields.Value("SizeX", 256.0f);
    terrainComp.SizeZ = fields.Value("SizeZ", 256.0f);
    terrainComp.HeightScale = fields.Value("HeightScale", 64.0f);
    terrainComp.SamplesPerMeter = fields.Value("SamplesPerMeter", terrainComp.SamplesPerMeter);
    terrainComp.StreamingRadius = fields.Value("StreamingRadius", terrainComp.StreamingRadius);
    terrainComp.Domain = fields.Value("Domain", 0) == 1
                             ? Components::TerrainDomain::Spherical
                             : Components::TerrainDomain::Planar;
    terrainComp.PlanetRadius = fields.Value("PlanetRadius", terrainComp.PlanetRadius);
    terrainComp.TargetPixelError = fields.Value("TargetPixelError", terrainComp.TargetPixelError);
    terrainComp.SeaLevel = fields.Value("SeaLevel", terrainComp.SeaLevel);
    terrainComp.CastShadows = fields.Value("CastShadows", terrainComp.CastShadows);
    terrainComp.ReceiveShadows = fields.Value("ReceiveShadows", terrainComp.ReceiveShadows);
    const bool terrainEnabled = fields.Value("Enabled", true);

    // Base relief is a companion component now (attached only to a planet). Accept the
    // legacy PlanetRelief* keys on the create payload so existing IPC callers keep working.
    Components::TerrainPlanetRelief reliefComp{};
    reliefComp.Amplitude = fields.Value("PlanetReliefAmplitude", reliefComp.Amplitude);
    reliefComp.Frequency = fields.Value("PlanetReliefFrequency", reliefComp.Frequency);
    reliefComp.Octaves = fields.Value("PlanetReliefOctaves", reliefComp.Octaves);

    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("create_entity", "Terrain", unconsumed);

    // Shared with the create menu (TerrainECS::ProvisionTerrainEntity says what it does: the
    // terrain's components and data, the scene's default surface-rules volume, and a refusal
    // for a size past the extent limit, which the caller gets back).
    if (std::string refusal = Editor::ProvisionTerrainEntity(*world, entity, terrainComp, reliefComp); !refusal.empty())
        return Editor::RefuseRequest(refusal);
    if (!terrainEnabled)
        ECS::Entity(world, entity).SetEnabled<Components::Terrain>(false);

    return json{};
}

// create_entity's write: a new entity with its name, its transform from the top-level
// position/rotation/scale, its parent and the requested components. Returns a refusal or a
// null json; `outEntity` is the new entity either way. Every component lands immediately, so
// the undo step committed after it captures them.
[[maybe_unused]] static json BuildRequestedEntity(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                 ECS::EntityHandle& outEntity)
{
    auto entity = world->Create();
    outEntity = entity.GetHandle();

    // Set name if provided.
    std::string name = params.value("name", "");
    if (!name.empty())
    {
        Components::Name nameComp{};
        std::strncpy(nameComp.value, name.c_str(), sizeof(nameComp.value) - 1);
        nameComp.value[sizeof(nameComp.value) - 1] = '\0';
        world->AddComponentImmediate(entity.GetHandle(), nameComp);
    }

    // Always add a Transform. Top-level position/rotation/scale default to identity.
    Mathematics::Vector3 pos{};
    Mathematics::Quaternion rot{};
    Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
    if (params.contains("position") && params["position"].is_object())
    {
        auto& p = params["position"];
        pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
    }
    if (params.contains("rotation") && params["rotation"].is_object())
    {
        auto& r = params["rotation"];
        rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
    }
    if (params.contains("scale") && params["scale"].is_object())
    {
        auto& s = params["scale"];
        scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
    }
    world->AddComponentImmediate(entity.GetHandle(), Components::Transform::FromTRS(pos, rot, scl));

    if (parentHandle.IsValid())
        world->AddComponentImmediate(entity.GetHandle(), Components::Parent{parentHandle});

    // Set components if provided.
    if (params.contains("components") && params["components"].is_object())
    {
        auto& comps = params["components"];
        if (comps.contains("Terrain"))
        {
            const auto& terrainValues = comps["Terrain"].is_object() ? comps["Terrain"] : json::object();
            auto err = CreateDefaultTerrainForEntity(world, entity.GetHandle(), terrainValues);
            if (!err.is_null())
                return err;
        }

        for (auto it = comps.begin(); it != comps.end(); ++it)
        {
            // Transform is already set from top-level position/rotation/scale.
            if (it.key() == "Transform" || it.key() == "Terrain")
                continue;
            const auto& values = it.value().is_object() ? it.value() : json::object();
            // SuppressNotify is irrelevant here: create_entity announces the
            // whole batch with one WorldStructureChanged.
            auto applied = ApplyComponentValues(world, entity.GetHandle(), it.key(), values);
            if (!applied.Error.is_null())
                return applied.Error;
        }
    }
    return json{};
}

// A snapshot edit of the spline `entity` already has, for a set_component SplineComponent that
// rewrites it in place; inactive when the entity has no live spline.
[[maybe_unused]] static Editor::UndoRedoService::InteractiveEdit BeginExistingSplineEdit(Editor::UndoRedoService& undo, ECS::World* world,
                                                                        ECS::EntityHandle entity,
                                                                        Editor::EditorChangeNotifications* notifications,
                                                                        const std::string& label)
{
    auto* service = SplineECS::SplineService::TryGet();
    const auto* spline = world->GetComponent<Components::SplineComponent>(entity);
    if (!service || !spline)
        return {};
    const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
    if (!service->IsValid(handle))
        return {};
    return undo.BeginInteractiveEdit(
        label, Editor::SplineUndo::MakeSplineEditableSnapshotTarget(service, handle, world, entity, notifications, label));
}

// set_component's write as one undo step (with `undo`): the entity's components before and after
// (CommitEntityComponentsEdit) and, for a SplineComponent, the points of the spline it rewrites
// in place, which live in the spline service rather than in component bytes. A write refused
// part-way records the keys it applied before refusing; one refused before writing records nothing.
[[maybe_unused]] static ComponentApplyResult CommitComponentWrite(Editor::UndoRedoService* undo,
                                                 Editor::EditorChangeNotifications* notifications, ECS::World* world,
                                                 ECS::EntityHandle entity, const std::string& componentName,
                                                 const json& values)
{
    const std::string label = "Set " + componentName;
    ComponentApplyResult applied;
    const auto write = [&]() {
        (void)Editor::CommitEntityComponentsEdit(*world, entity, undo, notifications, label, [&]() {
            applied = ApplyComponentValues(world, entity, componentName, values);
            return applied.Error.is_null();
        });
    };
    if (!undo)
    {
        write();
        return applied;
    }
    Editor::CommitGenericEdit(*world, *undo, label, [&]() {
        Editor::UndoRedoService::InteractiveEdit splineEdit;
        if (componentName == "SplineComponent")
            splineEdit = BeginExistingSplineEdit(*undo, world, entity, notifications, label);
        write();
        splineEdit.Commit();
    });
    return applied;
}

// BuildRequestedEntity, or nothing: a refused build removes the entity it began, so the refusal
// leaves the world as it was before the request.
[[maybe_unused]] static json BuildRequestedEntityOrNone(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                       ECS::EntityHandle& outEntity)
{
    json refusal = BuildRequestedEntity(world, params, parentHandle, outEntity);
    if (!refusal.is_null() && world->IsValid(outEntity))
        world->DestroyEntityImmediate(outEntity);
    return refusal;
}

[[maybe_unused]] static std::string EntityNameFromWorld(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid())
        return {};

    auto* name = world->GetComponent<Components::Name>(entity);
    if (name)
        return std::string(name->View());
    return {};
}

// In-place gamma encode (linear → sRGB) for pixels already quantized to RGBA8.
// Only for sources that are 8-bit to begin with (no precision left to recover).
// Float sources must encode in float inside ConvertToRGBA8 BEFORE the quantize:
// encoding after quantizing spreads one 1/255 linear quantum across ~4 output
// LSB near black, manufacturing banding the screen never shows.
[[maybe_unused]] static void ApplySRGBEncodeRGBA8(std::vector<uint8_t>& rgba)
{
    auto encode = [](uint8_t v) -> uint8_t
    {
        float linear = static_cast<float>(v) / 255.0f;
        float srgb = (linear <= 0.0031308f)
                         ? 12.92f * linear
                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        srgb = std::max(0.0f, std::min(1.0f, srgb));
        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    };
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = encode(rgba[i + 0]);
        rgba[i + 1] = encode(rgba[i + 1]);
        rgba[i + 2] = encode(rgba[i + 2]);
        // alpha is left untouched
    }
}

// Interleaved gradient noise, ported verbatim from encode_srgb.frag.
// Coordinates are pixel centers (x+0.5, y+0.5) to mirror gl_FragCoord.
[[maybe_unused]] static float InterleavedGradientNoiseAt(uint32_t px, uint32_t py, float offset)
{
    auto fract = [](float v) -> float { return v - std::floor(v); };
    const float x = static_cast<float>(px) + 0.5f + offset;
    const float y = static_cast<float>(py) + 0.5f + offset * 0.7f;
    return fract(52.9829189f * fract(0.06711056f * x + 0.00583715f * y));
}

// Per-pixel triangular-PDF dither, ported verbatim from encode_srgb.frag
// (InterleavedGradientNoise + inverse-CDF remap). The screen's sRGB encode pass
// dithers its 8-bit quantize with exactly this pattern; captures quantizing the
// same linear source must match or they show staircase banding the display
// never had. Returns a value in (-1, 1).
[[maybe_unused]] static float TriangularDitherAt(uint32_t px, uint32_t py)
{
    const float u = InterleavedGradientNoiseAt(px, py, 0.0f);
    const float o = 2.0f * u - 1.0f;
    const float s = (o > 0.0f) ? 1.0f : ((o < 0.0f) ? -1.0f : 0.0f);
    return s * (1.0f - std::sqrt(std::max(0.0f, 1.0f - std::abs(o))));
}

// Gradient-aware deband on the encoded float plane — the CPU mirror of
// encode_srgb.frag's pre-dither DebandEncoded, kept so captures continue to
// match the screen (the same parity contract as TriangularDitherAt above):
// identical noise fields, tap geometry, iteration schedule, and all-channel
// threshold gate, with clamp-to-edge bilinear taps mirroring the pass's
// sampler. One deliberate divergence: the shader encodes each bilinear tap of
// the LINEAR source, this taps the pre-ENCODED plane — the two differ only by
// transfer-curve curvature across a tap's bilinear footprint, sub-LSB on the
// shallow gradients the gate opens for, and the gate is shut at real edges
// either way. `thresholdLsb` is the pass-published effective gate — the SAME
// volume/env resolution the screen's backbuffer encode used (see
// GetBackbufferOutputDebandThresholdLsb), so a volume toggle or a GE_DEBAND
// relaunch changes screen and captures together. Radius stays process-wide.
//
// Reached only on frames whose composite is still LINEAR — an HDR-display
// capture, where the terminal pass remains the single encode and debands
// full-frame. On SDR frames each world view debanded inside its own image, so the
// composite arrives finalized and the conversion below requantizes it without
// running either filter.
[[maybe_unused]] static std::vector<float> DebandEncodedPlane(
    const std::vector<float>& enc, uint32_t width, uint32_t height, float thresholdLsb)
{
    const float threshold = thresholdLsb / 255.0f;
    const float baseRadius = Rendering::Passes::GetOutputDebandSettings().RadiusPx;
    constexpr int kIterations = 3; // encode_srgb.frag kDebandIterations
    constexpr float kGolden = 0.61803399f;
    constexpr float kTwoPi = 6.2831853f;

    auto fract = [](float v) -> float { return v - std::floor(v); };
    auto sampleBilinear = [&](float sx, float sy, float rgb[3])
    {
        // Pixel-center coordinates in, clamp-to-edge bilinear out.
        const float fx = std::clamp(sx - 0.5f, 0.0f, static_cast<float>(width - 1));
        const float fy = std::clamp(sy - 0.5f, 0.0f, static_cast<float>(height - 1));
        const uint32_t x0 = static_cast<uint32_t>(fx);
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t x1 = std::min(x0 + 1, width - 1);
        const uint32_t y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const float* p00 = &enc[(static_cast<size_t>(y0) * width + x0) * 3];
        const float* p10 = &enc[(static_cast<size_t>(y0) * width + x1) * 3];
        const float* p01 = &enc[(static_cast<size_t>(y1) * width + x0) * 3];
        const float* p11 = &enc[(static_cast<size_t>(y1) * width + x1) * 3];
        for (int c = 0; c < 3; ++c)
        {
            const float top = p00[c] + (p10[c] - p00[c]) * tx;
            const float bot = p01[c] + (p11[c] - p01[c]) * tx;
            rgb[c] = top + (bot - top) * ty;
        }
    };

    std::vector<float> out(enc.size());
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const float u1 = InterleavedGradientNoiseAt(px, py, 5.0f);
            const float u2 = InterleavedGradientNoiseAt(px, py, 11.0f);
            const float cx = static_cast<float>(px) + 0.5f;
            const float cy = static_cast<float>(py) + 0.5f;
            float res[3];
            const float* center = &enc[(static_cast<size_t>(py) * width + px) * 3];
            res[0] = center[0];
            res[1] = center[1];
            res[2] = center[2];
            for (int i = 1; i <= kIterations; ++i)
            {
                const float r1 = fract(u1 + static_cast<float>(i) * kGolden);
                const float r2 = fract(u2 + static_cast<float>(i) * kGolden);
                const float dist = baseRadius * static_cast<float>(i) * (0.25f + 0.75f * r1);
                const float angle = kTwoPi * r2;
                const float ox = dist * std::cos(angle);
                const float oy = dist * std::sin(angle);
                float t0[3], t1[3], t2[3], t3[3];
                sampleBilinear(cx + ox, cy + oy, t0);
                sampleBilinear(cx - oy, cy + ox, t1);
                sampleBilinear(cx - ox, cy - oy, t2);
                sampleBilinear(cx + oy, cy - ox, t3);
                // Gate against the ORIGINAL center (mirrors DebandEncoded):
                // bounds total displacement to one threshold by construction.
                float avg[3];
                bool pass = true;
                for (int c = 0; c < 3; ++c)
                {
                    avg[c] = 0.25f * (t0[c] + t1[c] + t2[c] + t3[c]);
                    pass = pass && std::abs(avg[c] - center[c]) < threshold;
                }
                if (pass)
                {
                    res[0] = avg[0];
                    res[1] = avg[1];
                    res[2] = avg[2];
                }
            }
            float* dst = &out[(static_cast<size_t>(py) * width + px) * 3];
            dst[0] = res[0];
            dst[1] = res[1];
            dst[2] = res[2];
        }
    }
    return out;
}

// Encode-filter state a capture should mirror: the effective deband gate the
// screen's backbuffer encode declared. Snapshotted on the render thread right
// after the target window's encode declaration (the render callback) so a sibling
// window's later declaration can't clobber the value before the deferred
// conversion runs; null falls back to the process-wide publish.
struct CaptureEncodeFilterState
{
    float DebandThresholdLsb = 0.0f;
};

// Encoded-domain finish for float color planes (3 floats per pixel, linear,
// already range-scaled): transfer curve, optional deband, TPDF dither,
// quantize to RGBA8. Splitting the encode from the quantize (vs the inline
// toU8 in ConvertToRGBA8) lets the deband tap a fully-encoded plane the way
// the screen's shader taps its source; with the deband inactive the op order
// (curve, +dither, clamp-quantize) is identical to the inline path.
[[maybe_unused]] static void EncodeDitherQuantizePlane(
    std::vector<float>& plane, uint32_t width, uint32_t height,
    bool srgbEncode, bool dither, bool deband, const CaptureEncodeFilterState* filterState,
    std::vector<uint8_t>& rgba)
{
    // Captures always quantize to 8 bits whatever depth the swapchain runs at,
    // so the mirror's dither is sized to ITS OWN quantizer — not to the screen's.
    constexpr float kCaptureDitherLsb = 1.0f / 255.0f;
    // BOTH filters are conditioned on `srgbEncode`, which asks "does this
    // conversion own the transfer curve?" — and therefore also "has anything
    // quantized these values for a destination yet?". A linear source (an
    // HDR-display frame's composite, a float data plane) has not been, so the
    // mirror encodes it and runs the screen's filters. An already-encoded source
    // arrives FINALIZED: every world view applied the OETF, debanded and dithered
    // at the presented step inside its own image, and the chrome around it is CSS
    // bytes that quantize exactly. Filtering that a second time would displace
    // world pixels the screen kept and grain chrome the screen left byte-stable,
    // so the conversion is a pure requantize — which makes screen-vs-capture
    // parity exact on chrome by construction. The residual it accepts: an 8-bit
    // capture of a world dithered for a 10-bit screen keeps only a quarter of
    // that noise, so a shallow gradient can contour in the PNG where the screen
    // shows none. Fixing that belongs upstream (finalize at the capture's step),
    // not in a filter here that cannot tell world from chrome.
    if (srgbEncode)
    {
        for (float& v : plane)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    // The gate is the pass-published effective threshold (volume AND env
    // resolved), 0 whenever the screen's filter is off — captures and screen
    // stay in lockstep through every combination of lever and override.
    const float effectiveDebandLsb =
        filterState ? filterState->DebandThresholdLsb
                    : Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();
    if (deband && srgbEncode && effectiveDebandLsb > 0.0f)
        plane = DebandEncodedPlane(plane, width, height, effectiveDebandLsb);

    const bool ditherThisPlane = dither && srgbEncode;
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const size_t i = static_cast<size_t>(py) * width + px;
            const float d =
                ditherThisPlane ? TriangularDitherAt(px, py) * kCaptureDitherLsb : 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                const float v = plane[i * 3 + c] + d;
                rgba[i * 4 + c] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
            }
        }
    }
}

// Convert raw readback bytes into RGBA8 for PNG encoding.
// Handles depth (linearize 0-1 → grayscale), float (tonemap), and integer formats.
// srgbEncode applies the linear→sRGB transfer curve (same constants as
// ApplySRGBEncodeRGBA8) — in float before the quantize for float/packed sources,
// in 8-bit for sources that are already quantized.
// dither adds a 1-LSB (1/255) TPDF offset in output space right before any
// float→8-bit quantize, matching the screen's encode pass. It never applies to
// sources that are already 8-bit — there is no sub-LSB information to recover,
// noise would only corrupt them.
// deband mirrors the screen's pre-dither deband on sRGB-encoded float sources
// (see DebandEncodedPlane); like dither it recovers/reshapes sub-LSB structure
// and never applies to already-quantized sources.
[[maybe_unused]] static std::vector<uint8_t> ConvertToRGBA8(
    const void* raw, uint32_t width, uint32_t height,
    Rendering::TextureFormat fmt, float rangeMin, float rangeMax, bool srgbEncode,
    bool dither, bool deband, const CaptureEncodeFilterState* filterState = nullptr)
{
    using TF = Rendering::TextureFormat;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(pixelCount * 4, 255);

    constexpr float kDitherLsb = 1.0f / 255.0f;

    // d is the per-pixel TPDF offset (already scaled to the 1/255 output LSB;
    // 0 when dithering is off), applied after the transfer curve — the same
    // order as encode_srgb.frag (encode, dither in sRGB space, quantize).
    auto toU8 = [srgbEncode](float v, float d) -> uint8_t
    {
        if (srgbEncode)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        v += d;
        return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
    };

    auto ditherAt = [dither, width](size_t i) -> float
    {
        if (!dither)
            return 0.0f;
        const uint32_t px = static_cast<uint32_t>(i % width);
        const uint32_t py = static_cast<uint32_t>(i / width);
        return TriangularDitherAt(px, py) * kDitherLsb;
    };

    float scale = (rangeMax > rangeMin) ? 1.0f / (rangeMax - rangeMin) : 1.0f;

    if (fmt == TF::RGBA8_UNORM || fmt == TF::RGBA8_SRGB)
    {
        std::memcpy(rgba.data(), raw, pixelCount * 4);
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::BGRA8_UNORM || fmt == TF::BGRA8_SRGB)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2]; // B→R
            rgba[i * 4 + 1] = src[i * 4 + 1]; // G→G
            rgba[i * 4 + 2] = src[i * 4 + 0]; // R→B
            rgba[i * 4 + 3] = src[i * 4 + 3]; // A→A
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::D32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (src[i] - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D24_UNORM_S8_UINT || fmt == TF::X8_D24_UNORM_PACK32)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i] & 0x00FFFFFFu) / 16777215.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D16_UNORM)
    {
        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i]) / 65535.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R16G16B16A16_FLOAT)
    {
        // half-float: use bit manipulation to convert to float
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (halfToFloat(src[i * 4 + 0]) - rangeMin) * scale;
            plane[i * 3 + 1] = (halfToFloat(src[i * 4 + 1]) - rangeMin) * scale;
            plane[i * 3 + 2] = (halfToFloat(src[i * 4 + 2]) - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R16_FLOAT)
    {
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (halfToFloat(src[i]) - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t g = toU8((src[i] - rangeMin) * scale, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32G32B32A32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (src[i * 4 + 0] - rangeMin) * scale;
            plane[i * 3 + 1] = (src[i * 4 + 1] - rangeMin) * scale;
            plane[i * 3 + 2] = (src[i * 4 + 2] - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R11G11B10_FLOAT)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            // Unpack R11G11B10 — 11-bit R, 11-bit G, 10-bit B (all unsigned float)
            uint32_t bits = src[i];
            auto unpack11 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 6) & 0x1F;
                uint32_t mantissa = v & 0x3F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 64.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            auto unpack10 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 5) & 0x1F;
                uint32_t mantissa = v & 0x1F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 32.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            float r = (unpack11(bits & 0x7FF) - rangeMin) * scale;
            float g = (unpack11((bits >> 11) & 0x7FF) - rangeMin) * scale;
            float b = (unpack10((bits >> 22) & 0x3FF) - rangeMin) * scale;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    if (fmt == TF::R8_UNORM)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i];
            rgba[i * 4 + 1] = src[i];
            rgba[i * 4 + 2] = src[i];
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::RGB10A2_UNORM)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t bits = src[i];
            float r = static_cast<float>(bits & 0x3FF) / 1023.0f;
            float g = static_cast<float>((bits >> 10) & 0x3FF) / 1023.0f;
            float b = static_cast<float>((bits >> 20) & 0x3FF) / 1023.0f;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    // Fallback: copy raw bytes as RGBA8 (best effort).
    size_t bpp = Rendering::BytesPerPixel(fmt);
    if (bpp > 0)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        size_t copyBytes = std::min(bpp, size_t(4));
        for (size_t i = 0; i < pixelCount; ++i)
        {
            std::memcpy(&rgba[i * 4], &src[i * bpp], copyBytes);
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
    }
    return rgba;
}

// Crop an RGBA8 buffer to a sub-rectangle. Clamps the rect to the source extents.
// Returns an empty buffer if the rect is fully out of bounds.
[[maybe_unused]] static std::vector<uint8_t> CropRGBA8(
    const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
    int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t& outW, uint32_t& outH)
{
    outW = 0;
    outH = 0;
    if (srcW == 0 || srcH == 0 || w <= 0 || h <= 0)
        return {};

    // Clamp against the source bounds.
    int32_t x0 = std::max(0, x);
    int32_t y0 = std::max(0, y);
    int32_t x1 = std::min<int32_t>(static_cast<int32_t>(srcW), x + w);
    int32_t y1 = std::min<int32_t>(static_cast<int32_t>(srcH), y + h);
    if (x1 <= x0 || y1 <= y0)
        return {};

    const uint32_t cw = static_cast<uint32_t>(x1 - x0);
    const uint32_t ch = static_cast<uint32_t>(y1 - y0);
    std::vector<uint8_t> out(static_cast<size_t>(cw) * ch * 4);
    for (uint32_t row = 0; row < ch; ++row)
    {
        const uint8_t* srcRow = src.data() + (static_cast<size_t>(y0 + row) * srcW + x0) * 4;
        uint8_t* dstRow = out.data() + static_cast<size_t>(row) * cw * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
    }
    outW = cw;
    outH = ch;
    return out;
}

// Encode RGBA8 pixels as PNG in memory using stb_image_write.
[[maybe_unused]] static std::vector<uint8_t> EncodePNG(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pngData;
    auto writeFunc = [](void* context, void* data, int size)
    {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        auto* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    stbi_write_png_to_func(writeFunc, &pngData,
                           static_cast<int>(width), static_cast<int>(height),
                           4 /*RGBA*/, rgba, static_cast<int>(width * 4));
    return pngData;
}

// ---------------------------------------------------------------------------
// capture_resource support
// ---------------------------------------------------------------------------

// Base64 of the whole buffer travels over the debug-server socket; a multi-MB
// GPUScene buffer would stall the channel for a diagnostic.
[[maybe_unused]] static constexpr uint64_t kMaxBufferCaptureBytes = 1ull << 20;
// Decoded arrays are for eyeballing values, not bulk transfer.
[[maybe_unused]] static constexpr size_t kMaxDecodedElements = 4096;
// Raw texel captures ship bytes verbatim over the same socket.
[[maybe_unused]] static constexpr uint64_t kMaxRawCaptureBytes = 4ull << 20;
// A frame declares hundreds of resources; a miss reports a bounded sample.
[[maybe_unused]] static constexpr size_t kMaxReportedCandidates = 64;

// Reported in the capture response so a caller can tell a 1-channel depth map
// from a 4-channel colour target without parsing the format name.
[[maybe_unused]] static uint32_t FormatChannelCount(Rendering::TextureFormat fmt)
{
    using TF = Rendering::TextureFormat;
    switch (fmt)
    {
    case TF::R8_UNORM:
    case TF::R8_UINT:
    case TF::R8_SINT:
    case TF::R16_FLOAT:
    case TF::R16_UINT:
    case TF::R16_SINT:
    case TF::R32_FLOAT:
    case TF::R32_UINT:
    case TF::R32_SINT:
    case TF::D32_FLOAT:
    case TF::D16_UNORM:
    case TF::S8_UINT:
    case TF::X8_D24_UNORM_PACK32:
        return 1;
    case TF::R8G8_UNORM:
    case TF::R8G8_UINT:
    case TF::R8G8_SINT:
    case TF::R16G16_FLOAT:
    case TF::R16G16_UINT:
    case TF::R16G16_SINT:
    case TF::R32G32_FLOAT:
    case TF::R32G32_UINT:
    case TF::R32G32_SINT:
    case TF::D24_UNORM_S8_UINT:
    case TF::D32_SFLOAT_S8_UINT:
        return 2;
    case TF::R32G32B32_UINT:
    case TF::R32G32B32_SINT:
    case TF::R11G11B10_FLOAT:
        return 3;
    default:
        return 4;
    }
}

// A resource declared in the frame under inspection.
struct CaptureResourceMatch
{
    uint32_t Id = 0xFFFFFFFFu;
    bool IsTexture = true;
    std::string Name;
};

// Resolve a capture_resource `name` against the resources declared in `frame`.
//
// An exact name wins outright. Otherwise a case-insensitive substring match is
// accepted only when it is UNIQUE — a debug tool that silently picks one of
// several "SceneView.Color"-ish targets would attribute the wrong pixels, which
// is worse than failing. Both failure modes fill `outCandidates` so the caller
// can report what the frame actually declared instead of a bare "not found".
[[maybe_unused]] static bool ResolveCaptureResource(const Rendering::RenderGraph::RGFrame& frame,
                                   const std::string& requested,
                                   CaptureResourceMatch& outMatch,
                                   std::vector<std::string>& outCandidates)
{
    namespace RG = Rendering::RenderGraph;
    const auto& graph = frame.Graph();
    const size_t count = graph.ResourceCount();

    auto lower = [](std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(requested);

    std::vector<CaptureResourceMatch> partial;
    for (uint32_t id = 0; id < count; ++id)
    {
        const auto& desc = graph.ResourceDesc(id);
        if (!desc.Name)
            continue;
        if (desc.Kind == RG::RGResourceKind::AccelerationStructure)
            continue; // no bytes the graph owns to capture
        const bool isTexture = desc.Kind == RG::RGResourceKind::Texture;
        const std::string name = desc.Name;
        if (name == requested)
        {
            outMatch = CaptureResourceMatch{id, isTexture, name};
            return true;
        }
        if (lower(name).find(needle) != std::string::npos)
            partial.push_back(CaptureResourceMatch{id, isTexture, name});
    }

    if (partial.size() == 1)
    {
        outMatch = partial.front();
        return true;
    }

    // Ambiguous → the near misses are the useful report; empty → everything is.
    const auto& report = partial;
    if (!report.empty())
    {
        for (const auto& m : report)
            outCandidates.push_back(m.Name);
    }
    else
    {
        for (uint32_t id = 0; id < count; ++id)
            if (graph.ResourceDesc(id).Name)
                outCandidates.push_back(graph.ResourceDesc(id).Name);
    }
    return false;
}

// ---------------------------------------------------------------------------
// take_screenshot support
// ---------------------------------------------------------------------------

struct ScreenshotCropDesc
{
    bool valid = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    std::string label;
};

// Monotonic per-session sequence: every capture writes a fresh file so a stale
// PNG from an earlier (possibly failed) capture can never masquerade as the
// new one when a caller re-reads a remembered filePath. It restarts at 1 in
// every process, which is why the names live under a process-scoped directory
// (CaptureOutputDirectory) rather than in one shared per-machine folder.
[[maybe_unused]] static std::atomic<uint32_t> s_CaptureSequence{0};

// Material texture bindings still waiting on a decode, world-wide. Reported on
// every capture and by get_editor_state: a frame taken while this is non-zero
// shows bindless defaults (white albedo) for those materials, which on screen is
// indistinguishable from a material that never binds. A first material bind tags
// the texture's cook usage, so on a cold derived cache the window is as wide as
// the block-compression encode — seconds to minutes. Zero when the renderer is
// not up, which is the honest answer for a frame with no material binding at all.
[[maybe_unused]] static size_t PendingMaterialTextureBinds()
{
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        return rs->Textures().PendingMaterialTextureBindCount();
    return 0u;
}

// Encode and publish one owned capture. Each request gets its own file because
// completed readbacks can encode concurrently on different workers.
[[maybe_unused]] static json EncodeCapturePng(const std::vector<uint8_t>& rgba8, uint32_t width, uint32_t height,
                             const std::string& fileStem)
{
    auto png = EncodePNG(rgba8.data(), width, height);
    if (png.empty())
        return Editor::RefuseRequest("PNG encoding failed");

    const std::filesystem::path filePath = Editor::CaptureOutputDirectory() /
        (fileStem + "_" + std::to_string(++s_CaptureSequence) + ".png");
    std::ofstream output(filePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    output.close();
    if (!output)
        return Editor::RefuseRequest("Capture write failed: " + filePath.string());
    return json{{"filePath", filePath.string()}, {"pngBase64", Base64Encode(png.data(), png.size())}};
}

[[maybe_unused]] static json FinishResourceCaptureResult(Rendering::ViewReadbackResult readback,
                                        float rangeMin, float rangeMax, json metadata)
{
    // Named resources have no color-space stamp. Visualize their declared range
    // without a transfer curve; raw captures retain values outside that range.
    auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                readback.format, rangeMin, rangeMax,
                                /*srgbEncode*/ false, /*dither*/ false, /*deband*/ false);
    std::string safeName = metadata.at("resourceName").get<std::string>();
    for (char& character : safeName)
        if (character == '/' || character == '\\' || character == ':' || character == '?' ||
            character == '*' || character == ' ')
            character = '_';
    json result = EncodeCapturePng(rgba8, readback.width, readback.height, "resource_" + safeName);
    if (!Editor::IsRefusal(result))
        result.update(metadata);
    return result;
}

// Crop, encode, persist, and fill the take_screenshot response (either the
// success payload or a refusal). Touches no editor state, so the rendergraph
// path runs it on a job: the encode costs seconds for a full window in DebugFast.
// `method` records which path produced the pixels: "rendergraph" (GPU readback
// of the frame composite) or "printwindow" (OS-compositor picture of the window,
// served only to a request that passed allowWindowCapture).
// `pendingMaterialTextureBinds` records whether the frame's materials had
// finished binding — the same class of self-description as `method`, and the
// one that tells a transient frame from a broken one.
[[maybe_unused]] static void FinishScreenshotResult(json& outResult,
                                   std::vector<uint8_t>&& rgba8,
                                   uint32_t srcW, uint32_t srcH,
                                   const ScreenshotCropDesc& crop,
                                   const std::string& target,
                                   const char* method,
                                   size_t pendingMaterialTextureBinds)
{
    uint32_t outW = srcW;
    uint32_t outH = srcH;
    std::vector<uint8_t> finalPixels;
    if (crop.valid)
    {
        finalPixels = CropRGBA8(rgba8, srcW, srcH, crop.x, crop.y, crop.w, crop.h, outW, outH);
        if (finalPixels.empty())
        {
            outResult = Editor::RefuseRequest("Crop produced empty image (rect outside window for target='" + target + "')");
            return;
        }
    }
    else
    {
        finalPixels = std::move(rgba8);
    }

    outResult = EncodeCapturePng(finalPixels, outW, outH, "screenshot_" + target);
    if (Editor::IsRefusal(outResult))
        return;
    outResult.update(json{
        {"width", outW},
        {"height", outH},
        {"sourceWidth", srcW},
        {"sourceHeight", srcH},
        {"target", target},
        {"format", "png"},
        {"method", method},
        {"pendingMaterialTextureBinds", pendingMaterialTextureBinds}
    });
}

// Find a panel of type T in the editor's panel storage.
template <typename T>
[[maybe_unused]] static T* FindPanel(const std::vector<std::unique_ptr<UIElement>>& panels)
{
    for (auto& el : panels)
    {
        if (auto* p = dynamic_cast<T*>(el.get()))
            return p;
    }
    return nullptr;
}

[[maybe_unused]] static GraphPanel* ResolveDebugGraphPanel(std::string_view kindId = {})
{
    if (!kindId.empty())
    {
        if (GraphPanel* live = GraphPanel::FindLiveByKind(kindId))
            return live;
    }
    if (GraphPanel* last = GraphPanel::FindLiveLastOpened())
        return last;
    GraphPanel* first = nullptr;
    GraphPanel::ForEachLive([&first](GraphPanel& panel) {
        if (!first)
            first = &panel;
    });
    return first;
}

[[maybe_unused]] static AnimationWindowPanel* FindAnimationPanelOfKind(const std::vector<std::unique_ptr<UIElement>>& panels,
                                                      AnimationWindowPanel::PanelKind kind)
{
    for (auto& el : panels)
    {
        if (auto* p = dynamic_cast<AnimationWindowPanel*>(el.get()))
        {
            if (p->GetPanelKind() == kind)
                return p;
        }
    }
    return nullptr;
}

// Component checker table: maps component name to a predicate.
using ComponentChecker = std::function<bool(ECS::World*, ECS::EntityHandle)>;

[[maybe_unused]] static const std::unordered_map<std::string, ComponentChecker>& GetComponentCheckers()
{
    static const std::unordered_map<std::string, ComponentChecker> kCheckers = {
        {"Transform", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Transform>(e); }},
        {"Name", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Name>(e); }},
        {"Parent", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Parent>(e); }},
        {"MeshRenderer", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::MeshRenderer>(e); }},
        {"Camera", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Camera>(e); }},
        {"Light", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Light>(e); }},
        {"Terrain", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Terrain>(e); }},
    };
    return kCheckers;
}

// Serialize a dynamically-discovered component to JSON via its raw bytes.
//
// A component with a reflected field table reads back BY FIELD NAME, in the canonical
// PascalCase the reflected writer accepts, so get_entity_components output feeds straight back
// into set_component (terrain-authoring-design §9.1). Enum fields carry their enumerator name
// and AssetRef<> fields their GUID as compact hex — both forms set_component parses.
// Everything else (physics, any type the scanner skipped) keeps the opaque hex blob: without a
// field table there is nothing to name it with, and guessing types would be worse than saying so.
[[maybe_unused]] static json SerializeDynamicComponent(ECS::World* world, ECS::EntityHandle entity,
                                      ECS::ComponentTypeId typeId)
{
    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
        return json::object();
    if (bytes.empty())
        return json::object();

    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (!fields.empty())
    {
        std::string text;
        ECS::AppendComponentJson(text, fields, reinterpret_cast<const std::byte*>(bytes.data()));
        // The dump builds JSON text; parse once per component rather than mirror its decoding
        // ladder here. A discarded parse falls through to the hex blob below, so a decode gap
        // degrades to the old opaque answer instead of dropping the component silently.
        if (json parsed = json::parse(text, nullptr, false); !parsed.is_discarded())
            return parsed;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hexStr;
    hexStr.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        hexStr.push_back(kHex[b >> 4]);
        hexStr.push_back(kHex[b & 0xF]);
    }
    return json{{"size", bytes.size()}, {"data", hexStr}};
}

// A component's on/off state is its ECS::ComponentDisabled tag, reported as "enabled" in its
// entry, for a type that switches through the tag (ComponentRegistry::SwitchesThroughDisabledTag).
// A type that keeps its own Enabled field carries it in its own dump; a NotToggleable type has none.
[[maybe_unused]] static void AddComponentEnabledState(json& entry, ECS::World* world, ECS::EntityHandle entity,
                                     ECS::ComponentTypeId typeId)
{
    if (!entry.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return;
    entry["enabled"] = world->IsComponentEnabled(entity, typeId);
}

// Serialize all components of an entity to JSON.
// Hand-written serializers cover the components whose IPC shape is not their field layout
// (Transform's TRS, physics runtime handles); everything else in the entity's archetype goes
// through SerializeDynamicComponent, which names reflected fields and falls back to a hex blob
// only for types with no field table.
//
// Every hand-written block below SHADOWS the reflected shape, and each is a fixed list someone
// has to remember to extend — the same class that left TerrainGrass covering 41 of its 54 fields
// on both sides. They are not all justified by the Transform/physics reason; several are just
// older than the reflected path. Prefer deleting one on contact to adding another, and add one
// only when the component's IPC shape genuinely is not its field layout.
[[maybe_unused]] static json SerializeEntityComponents(ECS::World* world, ECS::EntityHandle entity)
{
    json components = json::object();

    // Track which type IDs have been serialized by the hand-written path.
    std::unordered_set<ECS::ComponentTypeId> serialized;

    if (world->HasComponent<Components::Transform>(entity))
    {
        auto* c = world->GetComponent<Components::Transform>(entity);
        auto pos = c->GetPosition();
        auto rot = c->GetRotation();
        auto scl = c->GetScale();
        const auto& rq = rot.GetGLM();
        components["Transform"] = {
            {"position", {{"x", pos.x}, {"y", pos.y}, {"z", pos.z}}},
            {"rotation", {{"x", rq.x}, {"y", rq.y}, {"z", rq.z}, {"w", rq.w}}},
            {"scale", {{"x", scl.x}, {"y", scl.y}, {"z", scl.z}}}};
        serialized.insert(ECS::GetComponentTypeId<Components::Transform>());
    }

    if (world->HasComponent<Components::Name>(entity))
    {
        auto* c = world->GetComponent<Components::Name>(entity);
        components["Name"] = {{"value", std::string(c->View())}};
        serialized.insert(ECS::GetComponentTypeId<Components::Name>());
    }

    if (world->HasComponent<Components::Parent>(entity))
    {
        auto* c = world->GetComponent<Components::Parent>(entity);
        components["Parent"] = {{"parentId", c->parent.id}};
        serialized.insert(ECS::GetComponentTypeId<Components::Parent>());
    }

    if (world->HasComponent<Components::MeshRenderer>(entity))
    {
        auto* c = world->GetComponent<Components::MeshRenderer>(entity);
        components["MeshRenderer"] = {
            {"meshId", c->meshId},
            {"renderLayerMask", c->renderLayerMask},
            {"castShadows", c->castShadows},
            {"receiveShadows", c->receiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::MeshRenderer>());
    }

    if (world->HasComponent<Components::Camera>(entity))
    {
        auto* c = world->GetComponent<Components::Camera>(entity);
        components["Camera"] = {
            {"perspective", c->Perspective},
            {"fovY", c->FovY},
            {"orthographicSize", c->OrthographicSize},
            {"nearZ", c->NearZ},
            {"farZ", c->FarZ},
            {"cullingMask", c->CullingMask},
            {"postProcessProfileId", c->PostProcessProfileId},
            {"postProcessMask", c->PostProcessMask}};
        serialized.insert(ECS::GetComponentTypeId<Components::Camera>());
    }

    if (world->HasComponent<Components::Light>(entity))
    {
        auto* c = world->GetComponent<Components::Light>(entity);
        components["Light"] = {
            {"type", static_cast<uint32_t>(c->Type)},
            {"color", {c->Color[0], c->Color[1], c->Color[2]}},
            {"intensity", c->Intensity},
            {"range", c->Range},
            {"innerAngle", c->InnerAngle},
            {"outerAngle", c->OuterAngle},
            {"areaShape", static_cast<uint32_t>(c->AreaShape)},
            {"areaWidth", c->AreaWidth},
            {"areaHeight", c->AreaHeight},
            {"areaRadius", c->AreaRadius},
            {"falloff", static_cast<uint32_t>(c->Falloff)},
            {"decay", c->Decay},
            {"castsLight", c->CastsLight},
            {"castsShadows", c->CastsShadows},
            {"cascadeCount", c->CascadeCount},
            {"shadowAngularDiameter", c->ShadowAngularDiameter},
            {"fogContribution", c->FogContribution},
            {"fogDensityBoost", c->FogDensityBoost},
            {"fogAnisotropy", c->FogAnisotropy},
            {"fogOriginFade", c->FogOriginFade}};
        serialized.insert(ECS::GetComponentTypeId<Components::Light>());
    }

    if (world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
    {
        auto* c = world->GetComponent<Components::ParticleCollisionEventsBuffer>(entity);
        json events = json::array();
        for (uint32_t i = 0; i < c->Count && i < Components::ParticleCollisionEventsBuffer::kMaxEvents; ++i)
        {
            const auto& e = c->Events[i];
            events.push_back({{"Emitter", e.Emitter.id},
                              {"ParticleId", e.ParticleId},
                              {"KilledParticle", e.KilledParticle},
                              {"Position", {e.Position[0], e.Position[1], e.Position[2]}},
                              {"Normal", {e.Normal[0], e.Normal[1], e.Normal[2]}},
                              {"Velocity", {e.Velocity[0], e.Velocity[1], e.Velocity[2]}},
                              {"Speed", e.Speed}});
        }
        components["ParticleCollisionEventsBuffer"] = {
            {"Count", c->Count},
            {"Events", std::move(events)}};
        serialized.insert(ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>());
    }

    if (world->HasComponent<Components::Terrain>(entity))
    {
        auto* c = world->GetComponent<Components::Terrain>(entity);
        components["Terrain"] = {
            {"BaseSource", static_cast<uint32>(c->BaseSource)},
            {"TerrainAsset",
             c->TerrainAssetGuid.IsNull() ? std::string{} : c->TerrainAssetGuid.ToGuid().ToString()},
            {"SizeX", c->SizeX},
            {"SizeZ", c->SizeZ},
            {"HeightScale", c->HeightScale},
            {"SamplesPerMeter", c->SamplesPerMeter},
            {"StreamingRadius", c->StreamingRadius},
            {"Domain", static_cast<uint32>(c->Domain)},
            {"PlanetRadius", c->PlanetRadius},
            {"SphereSculpt",
             c->SphereSculptGuid.IsNull() ? std::string{} : c->SphereSculptGuid.ToGuid().ToString()},
            {"TargetPixelError", c->TargetPixelError},
            {"SeaLevel", c->SeaLevel},
            {"MaterialTiling", c->MaterialTiling},
            {"MaterialLibrary", c->MaterialLibraryGuid.IsNull()
                                   ? std::string{}
                                   : c->MaterialLibraryGuid.ToGuid().ToString()},
            // The library slot each channel role binds. Per-role for the same reason the
            // per-layer keys below are per-layer: the schema takes layerRole0..3, so an
            // array key would have nothing to land on when this is fed back to set_component.
            {"LayerRole0", static_cast<uint32>(c->LayerRoleSlot[0])},
            {"LayerRole1", static_cast<uint32>(c->LayerRoleSlot[1])},
            {"LayerRole2", static_cast<uint32>(c->LayerRoleSlot[2])},
            {"LayerRole3", static_cast<uint32>(c->LayerRoleSlot[3])},
            {"LayerAlbedo0", c->LayerAlbedoTexture[0].IsNull() ? std::string{} : c->LayerAlbedoTexture[0].ToGuid().ToString()},
            {"LayerAlbedo1", c->LayerAlbedoTexture[1].IsNull() ? std::string{} : c->LayerAlbedoTexture[1].ToGuid().ToString()},
            {"LayerAlbedo2", c->LayerAlbedoTexture[2].IsNull() ? std::string{} : c->LayerAlbedoTexture[2].ToGuid().ToString()},
            {"LayerAlbedo3", c->LayerAlbedoTexture[3].IsNull() ? std::string{} : c->LayerAlbedoTexture[3].ToGuid().ToString()},
            // Per-layer, not packed: every other key here lowercases to the Terrain scene
            // schema's own property name, so get_entity_components output can be fed
            // straight back to set_component. A `LayerTiling` array and a `LayerHexTiling`
            // bitmask have no schema property to land on (it takes layerTiling0..3 and
            // layerHex0..3), which broke that round-trip for exactly these two fields.
            {"LayerTiling0", c->LayerTiling[0]},
            {"LayerTiling1", c->LayerTiling[1]},
            {"LayerTiling2", c->LayerTiling[2]},
            {"LayerTiling3", c->LayerTiling[3]},
            {"LayerHex0", (c->LayerHexTiling & 1u) != 0u},
            {"LayerHex1", (c->LayerHexTiling & 2u) != 0u},
            {"LayerHex2", (c->LayerHexTiling & 4u) != 0u},
            {"LayerHex3", (c->LayerHexTiling & 8u) != 0u},
            {"RenderLayerMask", c->RenderLayerMask},
            {"CastShadows", c->CastShadows},
            {"ReceiveShadows", c->ReceiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::Terrain>());
    }

    if (world->HasComponent<Components::TerrainPlanetRelief>(entity))
    {
        auto* c = world->GetComponent<Components::TerrainPlanetRelief>(entity);
        components["TerrainPlanetRelief"] = {
            {"Amplitude", c->Amplitude},
            {"Frequency", c->Frequency},
            {"Octaves", c->Octaves}};
        serialized.insert(ECS::GetComponentTypeId<Components::TerrainPlanetRelief>());
    }

    if (world->HasComponent<Components::SplineComponent>(entity))
    {
        auto* c = world->GetComponent<Components::SplineComponent>(entity);
        components["SplineComponent"] = {
            {"SplineDataIndex", c->SplineDataIndex},
            {"SplineDataGeneration", c->SplineDataGeneration},
            {"DefaultRadius", c->DefaultRadius}};
        serialized.insert(ECS::GetComponentTypeId<Components::SplineComponent>());
    }

    // Physics components live outside the component scanner's reflected glob
    // (see the set_component handler), so the dynamic fallback below cannot name
    // their fields. The runtime handles are the interesting part here: they say
    // which physics body actually drives this entity, which is what tells a
    // duplicated or revived entity apart from one aliasing another's body.
    if (world->HasComponent<Components::PhysicsBody>(entity))
    {
        auto* c = world->GetComponent<Components::PhysicsBody>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["PhysicsBody"] = {
            {"motionType", static_cast<uint32_t>(c->motionType)},
            {"mass", c->mass},
            {"initialized", c->initialized},
            {"bodyHandle", c->body.Value()},
            {"shapeHandle", c->shape.Value()},
            {"childShapeCount", c->childShapeCount},
            // The physics world's own verdict on those handles: a copied handle
            // can name a live body it does not own, so "valid" is not "mine".
            {"bodyValid", pw ? pw->IsBodyValid(c->body) : false},
            {"shapeValid", pw ? pw->IsShapeValid(c->shape) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::PhysicsBody>());
    }

    if (world->HasComponent<Components::CharacterController>(entity))
    {
        auto* c = world->GetComponent<Components::CharacterController>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["CharacterController"] = {
            {"motor", static_cast<uint32_t>(c->motor)},
            {"radius", c->radius},
            {"height", c->height},
            {"initialized", c->initialized},
            {"grounded", c->grounded},
            {"characterHandle", c->character.Value()},
            {"characterValid", pw ? pw->IsCharacterValid(c->character) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::CharacterController>());
    }

    // Dynamic fallback: serialize remaining components via the ComponentRegistry,
    // then report every component's own on/off state beside its fields.
    auto* archetype = world->GetEntityArchetype(entity);
    if (archetype)
    {
        for (auto typeId : archetype->GetSignature().GetComponents())
        {
            std::string name;
            if (const std::string_view reflectedName = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
                !reflectedName.empty())
            {
                name.assign(reflectedName);
            }
            else if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            {
                name = info->Name;
            }
            else
            {
                name = "Component_" + std::to_string(typeId);
            }
            // A component's switched-off tag is reported as its "enabled" below, not as an
            // entry of its own.
            if (name.starts_with(ECS::kComponentDisabledNamePrefix))
                continue;

            // Use short name (strip namespace prefix) for JSON keys.
            auto lastColon = name.rfind("::");
            if (lastColon != std::string::npos)
                name = name.substr(lastColon + 2);
            if (!serialized.count(typeId))
                components[name] = SerializeDynamicComponent(world, entity, typeId);
            AddComponentEnabledState(components[name], world, entity, typeId);
        }
    }

    return components;
}

[[maybe_unused]] static std::vector<std::string> ParseDebugBuildSceneList(const std::string& raw)
{
    std::vector<std::string> out;
    for (size_t start = 0; start < raw.size();)
    {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos)
            end = raw.size();

        std::string line = raw.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        if (first < line.size())
            line = line.substr(first);
        if (!line.empty())
            out.push_back(std::move(line));

        start = end + (end < raw.size() ? 1u : 0u);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Render-graph introspection helpers (shared by the get_render_graph_* tools).
// All read the MAIN window's live RGFrame at request time, which holds the
// previous frame's compiled graph (the next BeginFrame, which clears it, runs
// later in the frame loop) — a stable snapshot.
// ---------------------------------------------------------------------------
namespace
{
namespace RG = Rendering::RenderGraph;

RG::RGFrame* MainRgFrame(EditorApplication& app)
{
    if (app.m_Windows.empty() || !app.m_Windows[0])
        return nullptr;
    return app.m_Windows[0]->RenderGraphStream.Frame.get();
}

// Lifetime classes reported by get_render_graph_resources. The backbuffer is
// an external import too, so it must be tested before the external case.
const char* RgResourceLifetimeName(const RG::RGFrame& frame, RG::RGResourceId r)
{
    if (frame.IsBackbuffer(RG::RGTexture{r}))
        return "Swapchain";
    if (frame.Graph().IsImported(r))
        return "Persistent";
    if (frame.Graph().IsExternal(r))
        return "Imported";
    return "Transient";
}

const char* RgQueueName(RG::RGQueue q)
{
    switch (q)
    {
    case RG::RGQueue::Graphics: return "Graphics";
    case RG::RGQueue::Compute:  return "Compute";
    case RG::RGQueue::Transfer: return "Copy";
    }
    return "?";
}

const char* RgLayoutName(RG::RGImageLayout l)
{
    switch (l)
    {
    case RG::RGImageLayout::Undefined:       return "Undefined";
    case RG::RGImageLayout::General:         return "General";
    case RG::RGImageLayout::ColorAttachment: return "ColorAttachment";
    case RG::RGImageLayout::DepthAttachment: return "DepthAttachment";
    case RG::RGImageLayout::DepthReadOnly:   return "DepthReadOnly";
    case RG::RGImageLayout::ShaderReadOnly:  return "ShaderReadOnly";
    case RG::RGImageLayout::TransferSrc:     return "TransferSrc";
    case RG::RGImageLayout::TransferDst:     return "TransferDst";
    case RG::RGImageLayout::Present:         return "Present";
    }
    return "?";
}

const char* RgAccessName(RG::RGAccess a)
{
    switch (a)
    {
    case RG::RGAccess::Sampled:         return "Sampled";
    case RG::RGAccess::SampledCompute:  return "SampledCompute";
    case RG::RGAccess::SampledVertex:   return "SampledVertex";
    case RG::RGAccess::UniformRead:     return "Uniform";
    case RG::RGAccess::StorageRead:     return "StorageRead";
    case RG::RGAccess::IndirectRead:    return "Indirect";
    case RG::RGAccess::IndexRead:       return "Index";
    case RG::RGAccess::VertexRead:      return "Vertex";
    case RG::RGAccess::CopySrc:         return "CopySrc";
    case RG::RGAccess::DepthRead:       return "DepthRead";
    case RG::RGAccess::ColorLoad:       return "ColorLoad";
    case RG::RGAccess::AccelerationStructureRead: return "AccelerationStructureRead";
    case RG::RGAccess::ColorAttachment: return "ColorAttachment";
    case RG::RGAccess::DepthWrite:      return "DepthWrite";
    case RG::RGAccess::StorageWrite:    return "StorageWrite";
    case RG::RGAccess::CopyDst:         return "CopyDst";
    case RG::RGAccess::AccelerationStructureBuild: return "AccelerationStructureBuild";
    }
    return "?";
}

uint32_t RgBarrierCountForPass(const RG::RGGraph& g, RG::RGPassId p)
{
    uint32_t n = 0;
    for (const auto& batch : g.BarrierBatches())
        if (batch.Pass == p)
            n += batch.Count;
    return n;
}

bool IsVec3Array(const json& v)
{
    return v.is_array() && v.size() >= 3 &&
           v[0].is_number() && v[1].is_number() && v[2].is_number();
}

// Camera IPC param validation. A wrong-shaped param used to be silently
// ignored while the handler still returned ok:true — e.g. a position sent as
// {"x": ...} instead of [x, y, z] left the camera unmoved with no error.
// Wrong shapes on recognized params are hard errors; unrecognized keys are
// collected as warnings so newer clients keep working against older editors.
// Returns a RefuseRequest refusal on shape mismatch, null json otherwise.
json ValidateCameraParams(const json& params,
                          std::initializer_list<const char*> vec3Keys,
                          std::initializer_list<const char*> numberKeys,
                          std::initializer_list<const char*> unsignedKeys,
                          json& outWarnings)
{
    outWarnings = json::array();
    if (!params.is_object())
        return Editor::RefuseRequest("params must be a JSON object");

    for (const char* key : vec3Keys)
    {
        if (params.contains(key) && !IsVec3Array(params[key]))
            return Editor::RefuseRequest(std::string("'") + key + "' must be a [x, y, z] number array");
    }
    for (const char* key : numberKeys)
    {
        if (params.contains(key) && !params[key].is_number())
            return Editor::RefuseRequest(std::string("'") + key + "' must be a number");
    }
    for (const char* key : unsignedKeys)
    {
        if (params.contains(key) && !params[key].is_number_unsigned())
            return Editor::RefuseRequest(std::string("'") + key + "' must be a non-negative integer");
    }

    auto isKnown = [&](const std::string& key)
    {
        for (const char* k : vec3Keys)
            if (key == k)
                return true;
        for (const char* k : numberKeys)
            if (key == k)
                return true;
        for (const char* k : unsignedKeys)
            if (key == k)
                return true;
        return false;
    };
    for (auto it = params.begin(); it != params.end(); ++it)
    {
        if (!isKnown(it.key()))
            outWarnings.push_back("ignored unknown param '" + it.key() + "'");
    }

    return json{};
}
} // namespace

// ---------------------------------------------------------------------------
// View activation, shared by focus_view and set_play_mode's activateGameView so
// the two can never drift into different notions of "that view is up".
// ---------------------------------------------------------------------------
namespace
{

struct ViewActivation
{
    // False only when the main window or its UI manager is unavailable; the
    // caller reports that rather than claiming a view it never raised.
    bool ok = false;
    // Read back from the dock model and the UI manager after the fact, never
    // assumed from the request.
    bool active = false;
    bool viewportFocused = false;
    std::string focusedElementId;
};

// Bring `panelId` to the front of its dock tab group and apply the UI focus that
// view wants. The two viewports want opposite things from UI focus — the Game
// View's viewport must hold it for input to reach the running game, the Scene
// View's must not or UIManager consumes its camera keys before the editor input
// actions see them — so the policy travels with the request, not with the panel.
ViewActivation ActivateDockedView(EditorApplication& app,
                                  DockingManager& docking,
                                  const std::string& panelId,
                                  bool wantsViewportUiFocus)
{
    ViewActivation result;
    if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->ui)
        return result;

    app.OpenPanel(panelId);

    auto* mainWindow = app.m_Windows[0].get();
    UIManager* ui = mainWindow->ui.get();

    // Raise the OS window: a backgrounded editor renders black or throttled, and
    // both callers exist to decide what gets rendered and captured next.
    if (mainWindow->window)
        mainWindow->window->Focus();

    if (wantsViewportUiFocus)
    {
        if (UIElement* rootEl = ui->GetRootElement())
        {
            if (rootEl->FindById(EditorPanelIds::GameViewViewport))
            {
                ui->SetFocusById(EditorPanelIds::GameViewViewport);
                result.viewportFocused = true;
            }
        }
    }
    else
    {
        ui->SetFocusById("");
    }

    result.ok = true;
    result.active = docking.IsPanelActiveTab(panelId);
    result.focusedElementId = ui->GetFocusedElementId();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

    static constexpr const char* kRgNoFrameMsg =
        "No live render graph (headless, or no frame rendered yet)";

    [[maybe_unused]] static auto RgTimingMap(RG::RGFrame* frame)
    {
        FlatMap<std::string, RG::RGFrame::RGPassTiming> timings;
        if (!frame)
            return timings;
        const auto resolvedTimings = frame->LastFrameTimings();
        timings.Reserve(resolvedTimings.size());
        for (const auto& timing : resolvedTimings)
            timings.InsertOrAssign(timing.Name, timing);
        return timings;
    }


} // namespace GameEngine
