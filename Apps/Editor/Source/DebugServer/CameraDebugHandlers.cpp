#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterCameraDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("get_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string view = ctx.params.value("view", std::string("scene"));
        if (view != "scene" && view != "game")
            return Editor::RefuseRequest("view must be 'scene' or 'game'");
        if (view == "game")
        {
            if (app.m_Windows.empty() || !app.m_Windows[0]->gameView)
                return Editor::RefuseRequest("No game view");
            const auto& game = *app.m_Windows[0]->gameView;
            const auto& camera = game.GetDeclaredCamera();
            if (!camera)
                return Editor::RefuseRequest("No declared game camera frame");
            auto result = Editor::DescribeGameViewCamera(camera->Data, camera->ExposureMode,
                                                         camera->Width, camera->Height);
            result["view"] = "game";
            result["viewId"] = static_cast<uint32_t>(game.GetViewId());
            result["source"] = "last_declared";
            result["frameIndex"] = camera->FrameIndex;
            result["waitingForExtraction"] = game.IsWaitingForExtraction();
            if (camera->ExposureMode == Components::ExposureMode::Auto)
            {
                if (auto* services = EngineCore::GetInstance().GetRenderServices())
                {
                    if (auto* readback = services->GetFeature<Engine::Renderer::ExposureReadbackFeature>())
                    {
                        float scale = 0.0f;
                        uint64_t frameIndex = 0;
                        if (readback->TryResolveAdaptedExposure(game.GetViewId(), scale, &frameIndex))
                            result["exposure"]["metered"] = Editor::DescribeMeteredExposure(scale, frameIndex);
                    }
                }
            }
            return result;
        }
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        // Null while no frame has been recorded, never a stand-in value: a
        // fabricated 60 degrees is the guess this field exists to retire.
        json projection = json(nullptr);
        float proj[16] = {};
        uint32_t viewW = 0;
        uint32_t viewH = 0;
        if (scene->GetLastFrameProjection(proj, viewW, viewH))
            projection = Editor::DescribeCameraProjection(proj, viewW, viewH);

        // is2D separates the two ways a projection turns orthographic: 2D mode,
        // which also makes the view ignore the 3D pose reported here, and a 3D
        // orthographic view, which does not. The projection type alone conflates
        // them, and the difference decides whether the pose above means anything.
        return json{
            {"position", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance},
            {"is2D", pose.Is2D},
            {"projection", std::move(projection)}
        }; });

server.RegisterHandler("set_scene_view_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Scene View 2D/3D mode. A 2D (orthographic) scene view ignores the 3D
        // camera pose, which silently breaks camera-driven automation — the
        // integration harness forces 3D before framing screenshots.
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");
        auto* scene = app.m_Windows[0]->scene.get();
        const bool was2D = scene->Is2DMode();
        if (ctx.params.contains("is2D"))
        {
            const bool want2D = ctx.params["is2D"].get<bool>();
            if (want2D != was2D)
                scene->Set2DMode(want2D);
        }
        return json{{"ok", true}, {"was2D", was2D}, {"is2D", scene->Is2DMode()}}; });

server.RegisterHandler("set_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        json warnings;
        if (json error = ValidateCameraParams(ctx.params,
                                              {"position"},
                                              {"yawDeg", "pitchDeg", "distance"},
                                              {},
                                              warnings);
            !error.is_null())
            return error;

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        if (ctx.params.contains("position"))
        {
            auto& p = ctx.params["position"];
            pose.Pos[0] = p[0].get<float>();
            pose.Pos[1] = p[1].get<float>();
            pose.Pos[2] = p[2].get<float>();
        }
        if (ctx.params.contains("yawDeg"))   pose.YawDeg   = ctx.params["yawDeg"].get<float>();
        if (ctx.params.contains("pitchDeg")) pose.PitchDeg = ctx.params["pitchDeg"].get<float>();
        if (ctx.params.contains("distance")) pose.Distance = ctx.params["distance"].get<float>();

        scene->SetCameraPose(pose);

        // Push the new angles into the SceneViewPanel as well. Each frame
        // RecordWindowWorldViews overwrites the controller's yaw/pitch from
        // the panel via SetCameraAnglesDeg — without this sync, our MCP-set
        // angles would be clobbered on the next frame.
        if (auto* root = app.m_Windows[0]->ui->GetRootElement())
            if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                    svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);

        json response{
            {"ok", true},
            {"position", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance}
        };
        if (!warnings.empty())
            response["warnings"] = warnings;
        return response; });

server.RegisterHandler("look_at", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        json warnings;
        if (json error = ValidateCameraParams(ctx.params,
                                              {"target", "direction"},
                                              {"distance"},
                                              {"entityId"},
                                              warnings);
            !error.is_null())
            return error;

        float target[3] = {0.0f, 0.0f, 0.0f};
        bool framedBounds = false;
        float autoDistance = 0.0f; // subtree-derived; used when the caller omits distance

        // Target can be explicit coordinates or derived from an entity.
        if (ctx.params.contains("entityId"))
        {
            uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
            auto* world = EngineCore::GetInstance().GetPrimaryWorld();
            if (!world)
                return Editor::RefuseRequest("No world available");

            ECS::EntityHandle entity(entityId);
            if (!world->IsValid(entity))
                return Editor::RefuseRequest("Invalid entity");

            // Frame the subtree bounds union: a large model's Transform origin
            // can sit inside its own geometry, and a fixed default distance
            // used to place the camera inside the mesh.
            Editor::SubtreeWorldBounds subtree;
            if (Editor::ComputeSubtreeWorldBounds(*world, entity, subtree) && subtree.HasBounds)
            {
                const Mathematics::Vector3 center = subtree.Center();
                target[0] = center.x;
                target[1] = center.y;
                target[2] = center.z;
                autoDistance = std::max(subtree.Radius() * 1.8f, 0.5f);
                framedBounds = true;
            }
            else if (auto* xf = world->GetComponent<Components::Transform>(entity))
            {
                auto pos = xf->GetPosition();
                target[0] = pos.x;
                target[1] = pos.y;
                target[2] = pos.z;
            }
        }
        else if (ctx.params.contains("target"))
        {
            auto& t = ctx.params["target"];
            target[0] = t[0].get<float>();
            target[1] = t[1].get<float>();
            target[2] = t[2].get<float>();
        }
        else
        {
            return Editor::RefuseRequest("Provide 'entityId' or 'target' [x,y,z]");
        }

        // Explicit distance always wins; otherwise fit the framed bounds, or
        // fall back to the historical default when no bounds exist.
        float distance = ctx.params.value("distance", framedBounds ? autoDistance : 5.0f);

        // Direction: default to looking from front-right-above if not specified.
        float dirX = 1.0f, dirY = -0.5f, dirZ = 1.0f;
        if (ctx.params.contains("direction"))
        {
            auto& d = ctx.params["direction"];
            dirX = d[0].get<float>();
            dirY = d[1].get<float>();
            dirZ = d[2].get<float>();
        }

        SceneViewCameraPose pose{};
        if (!Editor::ComputeLookAtPose(Mathematics::Vector3(target[0], target[1], target[2]),
                                       Mathematics::Vector3(dirX, dirY, dirZ), distance, pose))
            return Editor::RefuseRequest("Direction vector is zero");
        const float camX = pose.Pos[0];
        const float camY = pose.Pos[1];
        const float camZ = pose.Pos[2];
        const float yawDeg = pose.YawDeg;
        const float pitchDeg = pose.PitchDeg;

        ApplyMainSceneViewPose(app, pose);

        json response{
            {"ok", true},
            {"position", {camX, camY, camZ}},
            {"target", {target[0], target[1], target[2]}},
            {"yawDeg", yawDeg},
            {"pitchDeg", pitchDeg},
            {"distance", distance},
            {"framedBounds", framedBounds}
        };
        if (!warnings.empty())
            response["warnings"] = warnings;
        return response; });

server.RegisterHandler("move_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        if (ctx.params.contains("position") && ctx.params["position"].is_array())
        {
            auto& p = ctx.params["position"];
            if (p.size() >= 3)
            {
                pose.Pos[0] = p[0].get<float>();
                pose.Pos[1] = p[1].get<float>();
                pose.Pos[2] = p[2].get<float>();
            }
        }
        if (ctx.params.contains("yawDeg"))   pose.YawDeg   = ctx.params["yawDeg"].get<float>();
        if (ctx.params.contains("pitchDeg")) pose.PitchDeg = ctx.params["pitchDeg"].get<float>();
        if (ctx.params.contains("distance")) pose.Distance = ctx.params["distance"].get<float>();

        float duration = ctx.params.value("durationSec", 0.7f);
        scene->StartCameraTween(pose, duration);

        return json{
            {"ok", true},
            {"tweening", true},
            {"targetPosition", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance},
            {"durationSec", duration}
        }; });
}
}
