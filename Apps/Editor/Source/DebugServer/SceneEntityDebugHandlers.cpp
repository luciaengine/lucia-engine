#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterSceneEntityDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("get_scene_hierarchy", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return json{{"entities", json::array()}};
        return Editor::DescribeSceneHierarchy(*world); });

server.RegisterHandler("get_entity_components", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        json result;
        result["entityId"] = entityId;
        result["components"] = SerializeEntityComponents(world, entity);
        return result; });

server.RegisterHandler("query_ecs", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        std::string componentName = ctx.params.value("hasComponent", "");
        int limit = ctx.params.value("limit", 50);

        if (componentName.empty())
            return Editor::RefuseRequest("Missing hasComponent parameter");

        // Try the fast static checker table first, then fall back to reflected
        // field metadata and the older ComponentRegistry.
        auto& checkers = GetComponentCheckers();
        auto it = checkers.find(componentName);

        // Dynamic fallback: collect ALL type IDs whose registered name matches.
        // There can be multiple (e.g. C++ auto-registered + scripting blob component).
        std::vector<ECS::ComponentTypeId> matchingTypeIds;
        if (it == checkers.end())
        {
            if (const ECS::ComponentTypeId reflectedTypeId =
                    ECS::ComponentFieldRegistry::FindByName(componentName);
                reflectedTypeId != 0)
            {
                matchingTypeIds.push_back(reflectedTypeId);
            }

            const std::string suffix = "::" + componentName;
            for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
            {
                bool match = (regName == componentName) ||
                             (regName.size() >= suffix.size() &&
                              regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
                if (match)
                {
                    auto tid = ECS::ComponentRegistry::GetComponentTypeId(regName);
                    if (tid != 0)
                        matchingTypeIds.push_back(tid);
                }
            }
            if (matchingTypeIds.empty())
                return Editor::RefuseRequest("Unknown component: " + componentName);
        }

        auto entities = world->GetAliveEntitiesSnapshot();
        json results = json::array();
        int count = 0;

        for (auto entity : entities)
        {
            if (count >= limit)
                break;

            bool has = false;
            if (it != checkers.end())
            {
                has = it->second(world, entity);
            }
            else
            {
                auto* archetype = world->GetEntityArchetype(entity);
                if (archetype)
                {
                    const auto& sig = archetype->GetSignature();
                    for (auto tid : matchingTypeIds)
                    {
                        if (sig.Contains(tid))
                        {
                            has = true;
                            break;
                        }
                    }
                }
            }

            if (has)
            {
                json entry;
                entry["id"] = entity.id;
                entry["name"] = EntityNameFromWorld(world, entity);
                results.push_back(std::move(entry));
                ++count;
            }
        }

        return json{{"entities", std::move(results)}, {"total", count}}; });

server.RegisterHandler("create_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        // Validate before Create so a bad request doesn't leave an orphan entity behind.
        ECS::EntityHandle parentHandle = ECS::EntityHandle::Invalid();
        if (ctx.params.contains("parentId"))
        {
            const uint32_t parentId = ctx.params.value("parentId", 0u);
            parentHandle = ECS::EntityHandle(parentId);
            if (parentId == 0 || !world->IsValid(parentHandle))
                return Editor::RefuseRequest("Invalid parentId");
        }

        // A port edit is one undo step: the entity is built and committed as already applied
        // (the markup_create precedent), so undo removes it and redo revives the same handle.
        // A refused build removes what it began and records nothing.
        constexpr const char* kCreateEntityLabel = "Create Entity";
        ECS::EntityHandle entity{};
        json refusal;
        if (app.m_UndoRedo)
        {
            Editor::CommitGenericEdit(*world, *app.m_UndoRedo, kCreateEntityLabel, [&]() {
                refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
                if (!refusal.is_null())
                    return;
                app.m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>(
                    kCreateEntityLabel, world, app.m_ChangeNotifications.get(), std::vector<ECS::EntityHandle>{entity},
                    nullptr, nullptr));
            });
        }
        else
        {
            refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
        }
        if (!refusal.is_null())
            return refusal;

        // Notify the editor so the hierarchy panel refreshes.
        if (app.m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = world;
            e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            app.m_ChangeNotifications->NotifyWorldStructureChanged(e);
        }

        return json{{"entityId", entity.id}}; });

server.RegisterHandler("set_component", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId") || !ctx.params.contains("component"))
            return Editor::RefuseRequest("Missing entityId or component parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        std::string componentName = ctx.params["component"].get<std::string>();

        // Accept values as an object or default to empty object (add component with defaults).
        json values = json::object();
        if (ctx.params.contains("values"))
        {
            auto& v = ctx.params["values"];
            if (v.is_object())
                values = v;
            else if (v.is_string())
            {
                // LLMs sometimes stringify the JSON object — parse it.
                auto parsed = json::parse(v.get<std::string>(), nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                    return Editor::RefuseRequest("values must be a JSON object (or a JSON string encoding one)");
                values = std::move(parsed);
            }
        }

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // A port edit is one undo step (CommitComponentWrite): the components the write added
        // or changed (a collider shape brings its PhysicsCollider) and a rewritten spline's points.
        const ComponentApplyResult applied = CommitComponentWrite(app.m_UndoRedo.get(), app.m_ChangeNotifications.get(),
                                                                  world, entity, componentName, values);
        if (!applied.Error.is_null())
            return applied.Error;

        // Notify like the editor's own edit paths so event-driven UI (inspector
        // fields, tool availability gates) reacts to debug-driven writes.
        // UndoRedo kind = "values changed outside the inspector UI; rebuild"
        // (the drop-command precedent). Data-only fast paths (Parent) opt out
        // via the apply result — the policy lives in ApplyComponentValues next
        // to the branch that implements it.
        if (app.m_ChangeNotifications && !applied.SuppressNotify)
        {
            Editor::EditorChangeNotifications::ComponentChangedEvent e{};
            e.world = world;
            e.entity = entity;
            e.componentType = ResolveComponentTypeIdByName(componentName);
            e.kind = Editor::EditorChangeNotifications::ChangeKind::UndoRedo;
            app.m_ChangeNotifications->NotifyComponentChanged(e);
        }

        return json{{"ok", true}}; });

server.RegisterHandler("set_entity_enabled", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        const uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        const bool enabled = ctx.params.value("enabled", true);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        const ECS::EntityHandle entities[] = {entity};
        Editor::CommitEntityEnabledToggle(*world, app.m_UndoRedo.get(), app.m_ChangeNotifications.get(), entities,
                                          enabled);
        return json{{"ok", true}, {"enabled", enabled}}; });

server.RegisterHandler("delete_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        std::vector<ECS::EntityHandle> ents =
            Editor::DeleteEntitiesCommand::CollectSubtree(*world, entity);
        if (ents.empty())
            return Editor::RefuseRequest("Entity resolved to an empty subtree");

        const uint64_t deletedCount = static_cast<uint64_t>(ents.size());
        const std::string undoName = (ents.size() > 1) ? "Delete Entities" : "Delete Entity";

        if (app.m_UndoRedo)
        {
            app.m_UndoRedo->Execute(std::make_unique<Editor::DeleteEntitiesCommand>(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents)));
        }
        else
        {
            Editor::DeleteEntitiesCommand cmd(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents));
            cmd.Redo();
        }

        return json{{"ok", true},
                    {"deletedCount", deletedCount},
                    {"undoTracked", app.m_UndoRedo != nullptr},
                    {"undoName", undoName},
                    {"canUndo", app.CanUndo()}}; });

server.RegisterHandler("select_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // Route through SceneViewController which updates gizmos + Inspector.
        if (!app.m_Windows.empty() && app.m_Windows[0]->scene)
        {
            app.m_Windows[0]->scene->OnEntityPicked(entity);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        // Fallback: update the InspectorPanel directly.
        auto* inspector = FindPanel<InspectorPanel>(app.m_PanelStorage);
        if (inspector)
        {
            inspector->ShowEntity(world, entity, /*force=*/false, /*keepMultiSelection=*/false);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        return Editor::RefuseRequest("No scene view or inspector available"); });
}
}
