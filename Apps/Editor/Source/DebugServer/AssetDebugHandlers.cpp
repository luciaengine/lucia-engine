#include "DebugHandlerHelpers.h"

#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "Mathematics/Quaternion.h"

namespace GameEngine
{
void RegisterAssetDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
server.RegisterHandler("import_proflares", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string sourceDir = ctx.params.value("sourceDir", "");
        if (sourceDir.empty())
            return Editor::RefuseRequest("Provide sourceDir (absolute path to the ProFlares folder)");
        std::error_code ec;
        if (!std::filesystem::is_directory(sourceDir, ec))
            return Editor::RefuseRequest("sourceDir is not a directory: " + sourceDir);

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const std::filesystem::path projectRoot = am.GetSourceRoot(kAssetSourceAliasProject);
        if (projectRoot.empty())
            return Editor::RefuseRequest("No project asset root mounted");

        const std::string subDir = ctx.params.value("outputSubDir", std::string{"LensFlares"});
        const std::filesystem::path outputDir = projectRoot / subDir;

        Logger::Log::Info("import_proflares: scanning '{}' -> '{}'", sourceDir, outputDir.string());
        std::vector<LensFlareImport::FolderImportEntry> report;
        int written = 0;
        try
        {
            written = LensFlareImport::ImportFolder(sourceDir, outputDir, subDir, report);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("import_proflares: ImportFolder threw: {}", e.what());
            json entries = json::array();
            for (const auto& r : report)
                entries.push_back(json{{"source", r.Source.string()},
                                       {"note", r.Note},
                                       {"ok", r.Ok}});
            return Editor::RefuseRequest(std::string("ImportFolder threw: ") + e.what(),
                                         json{{"entriesBeforeThrow", std::move(entries)}});
        }
        Logger::Log::Info("import_proflares: wrote {} assets ({} report entries)", written,
                          report.size());

        json entries = json::array();
        for (const auto& e : report)
            entries.push_back(json{{"source", e.Source.string()},
                                   {"output", e.Output.string()},
                                   {"note", e.Note},
                                   {"ok", e.Ok}});
        return json{{"ok", true},
                    {"written", written},
                    {"outputDir", outputDir.string()},
                    {"entries", std::move(entries)}}; });

server.RegisterHandler("open_asset", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string requestedPath = ctx.params.value("path", std::string{});
        if (requestedPath.empty())
            return Editor::RefuseRequest("Provide path");
        const std::filesystem::path path =
            Editor::ResolveOpenAssetPath(EngineCore::GetInstance().GetAssetManager(), requestedPath);
        if (path.empty())
            return Editor::RefuseRequest("Cannot resolve asset path: " + requestedPath);

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".anim" || ext == ".animation" || ext == ".fbx" || ext == ".gltf" ||
            ext == ".glb" || ext == ".timeline" || ext == ".clipset")
        {
            auto* animationPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::Animation);
            auto* targetPanel = animationPanel;
            const char* targetPanelId = EditorPanelIds::Animation;
            if (ext == ".timeline")
            {
                targetPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::Timeline);
                targetPanelId = EditorPanelIds::Timeline;
            }
            else if (ext == ".clipset")
            {
                targetPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::ClipEditor);
                targetPanelId = EditorPanelIds::ClipEditor;
            }
            if (!targetPanel)
                targetPanel = animationPanel ? animationPanel : FindPanel<AnimationWindowPanel>(app.m_PanelStorage);
            if (!targetPanel)
                return Editor::RefuseRequest("Animation panel not found");
            if (app.m_PanelManager && !app.m_Windows.empty())
                app.m_PanelManager->ShowOrActivatePanel(app.m_Windows[0].get(), targetPanelId, EditorPanelIds::SceneView);
            if (!targetPanel->OpenAnimationAssetPath(path))
                return Editor::RefuseRequest("Animation panel failed to open asset");
            return json{{"ok", true}, {"path", path.string()}};
        }

        if (ExtensionOpensInGraphPanel(ext))
        {
            if (ext == ".glsl" && !Editor::FileIsShaderGraphGlsl(path))
            {
                return Editor::RefuseRequest("GLSL is not a shader graph file (use script editor instead)");
            }

            const auto started = std::chrono::steady_clock::now();
            if (!app.OpenGraphAsset(path))
                return Editor::RefuseRequest("Graph panel failed to open asset");

            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            GraphPanel* opened = GraphPanel::FindLiveLastOpened();
            const std::string kind = opened ? std::string(opened->PanelKindId())
                                            : KindIdFromGraphExtension(ext);
            std::string panelId;
            if (opened && !app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->docking)
            {
                for (const auto& [id, element] : app.m_Windows[0]->docking->GetPanels())
                {
                    if (element == opened)
                    {
                        panelId = id;
                        break;
                    }
                }
            }
            if (panelId.empty())
            {
                if (const char* kindDock = GraphDockPanelIdForKind(kind))
                    panelId = kindDock;
                else
                    panelId = EditorPanelIds::NodeGraph;
            }
            return json{{"ok", true},
                        {"path", path.string()},
                        {"panel", panelId},
                        {"kindId", kind},
                        {"openMs", elapsedMs}};
        }

        return Editor::RefuseRequest("Unsupported asset type: " + ext); });

server.RegisterHandler("select_smart_folder", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* assetsPanel = app.m_Docking
            ? dynamic_cast<AssetsPanel*>(app.m_Docking->GetPanel(EditorPanelIds::Assets))
            : nullptr;
        if (!assetsPanel)
            assetsPanel = FindPanel<AssetsPanel>(app.m_PanelStorage);
        if (!assetsPanel)
            return Editor::RefuseRequest("Assets panel not found");
        const std::string name = ctx.params.value("name", std::string{});
        if (name.empty())
            return Editor::RefuseRequest("Provide smart folder name");
        if (!assetsPanel->SelectSmartFolderByName(name))
            return Editor::RefuseRequest("Smart folder not found: " + name);
        return json{{"ok", true},
                    {"name", name},
                    {"itemCount", assetsPanel->GetVisibleAssetCount()},
                    {"directory", assetsPanel->GetVisibleAssetDirectory().string()}}; });

server.RegisterHandler("select_asset", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string requestedPath = ctx.params.value("path", std::string{});
        if (requestedPath.empty())
            return Editor::RefuseRequest("Provide path");
        const std::filesystem::path assetPath =
            Editor::ResolveOpenAssetPath(EngineCore::GetInstance().GetAssetManager(), requestedPath);
        if (assetPath.empty())
            return Editor::RefuseRequest("Cannot resolve asset path: " + requestedPath);

        auto* assetsPanel = FindPanel<AssetsPanel>(app.m_PanelStorage);
        if (!assetsPanel)
            return Editor::RefuseRequest("Assets panel not found");

        if (app.m_PanelManager && !app.m_Windows.empty())
        {
            auto* window = app.m_Windows[0].get();
            app.m_PanelManager->ShowOrActivatePanel(window, EditorPanelIds::Assets, EditorPanelIds::SceneView);
            app.m_PanelManager->ShowOrActivatePanel(window, EditorPanelIds::AssetView, EditorPanelIds::Assets);
        }

        const bool silent = ctx.params.value("silent", false);
        if (silent)
            assetsPanel->NavigateToAndSelectAssetSilent(assetPath);
        else
            assetsPanel->NavigateToAndSelectAsset(assetPath);

        json result{{"ok", true}, {"path", assetPath.string()}};
        if (auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage))
        {
            assetViewPanel->SetAssetPreview(assetPath, true);
            const auto preview = assetViewPanel->GetPreviewDebugState();
            result["assetView"] = {
                {"previewPath", preview.previewPath},
                {"isVideoPreview", preview.isVideoPreview},
                {"videoPlayerLoaded", preview.videoPlayerLoaded},
                {"videoTextureUploaded", preview.videoTextureUploaded},
                {"videoNeedsBackgroundBind", preview.videoNeedsBackgroundBind},
                {"videoTextureWidth", preview.videoTextureWidth},
                {"videoTextureHeight", preview.videoTextureHeight},
                {"videoTextureResourceName", preview.videoTextureResourceName},
            };
        }

        return result; });

server.RegisterHandler("get_asset_preview", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage);
        if (!assetViewPanel)
            return Editor::RefuseRequest("Asset View panel not found");

        const auto preview = assetViewPanel->GetPreviewDebugState();
        return json{
            {"panelId", EditorPanelIds::AssetView},
            {"previewElementId", "asset-view-preview-image"},
            {"previewPath", preview.previewPath},
            {"previewEnabled", preview.previewEnabled},
            {"isVideoPreview", preview.isVideoPreview},
            {"videoPlayerLoaded", preview.videoPlayerLoaded},
            {"videoTextureUploaded", preview.videoTextureUploaded},
            {"videoNeedsBackgroundBind", preview.videoNeedsBackgroundBind},
            {"videoTextureWidth", preview.videoTextureWidth},
            {"videoTextureHeight", preview.videoTextureHeight},
            {"videoTextureResourceName", preview.videoTextureResourceName},
            {"displayedEngineResource", preview.displayedEngineResource},
        }; });

server.RegisterHandler("get_asset_sources", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();
        auto sources = registry.GetRegisteredSources();
        json result = json::array();
        for (const auto& s : sources)
        {
            json src = json::object();
            src["alias"] = s.Alias;
            src["root"] = s.Root.string();
            src["priority"] = s.Priority;
            src["derivedIdentity"] = s.DerivedIdentity;
            auto guids = registry.GetAssetsForSource(s.Alias);
            src["assetCount"] = guids.size();
            result.push_back(std::move(src));
        }
        return json{{"sources", result}}; });

server.RegisterHandler("get_asset_list", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string sourceAlias = ctx.params.value("source", "project");
        int limit = ctx.params.value("limit", 50);

        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();
        auto guids = registry.GetAssetsForSource(sourceAlias);

        json assets = json::array();
        int count = 0;
        for (const auto& guid : guids)
        {
            if (count >= limit) break;
            AssetMetadata meta;
            if (registry.TryGetAssetMetadata(guid, meta))
            {
                json a = json::object();
                a["guid"] = guid.ToString();
                a["path"] = meta.Path.string();
                a["type"] = static_cast<int>(meta.Type);
                a["name"] = std::string(meta.Name);
                assets.push_back(std::move(a));
                ++count;
            }
        }
        return json{{"source", sourceAlias}, {"total", guids.size()}, {"assets", assets}}; });

server.RegisterHandler("get_skeleton_store_state", [](const EditorDebugServer::RequestContext& requestContext) -> json
                           {
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        const uint32_t skeletonCount = store.GetCount();

        // Resolve the optional GPU stores so we can label each runtime's
        // atlas owner. The skin-matrix probe below operates on the runtime's
        // CompactSkinMatrices (CPU cache), which is intentionally not
        // refreshed when GPU owns the slot — without an owner tag, the
        // identity-looking probe values can be misread as a rendering bug.
        auto* renderServices = EngineCore::GetInstance().GetRenderServices();
        const auto* gpuAnimation = renderServices ? &renderServices->GetGPUAnimationDataStore() : nullptr;
        const auto* retargetFeature = renderServices ? renderServices->GetFeature<Engine::Renderer::RetargetRenderFeature>() : nullptr;
        const auto* retargetStore = (retargetFeature && retargetFeature->IsInitialized())
            ? &retargetFeature->GetDataStore() : nullptr;

        auto* primaryWorld = EngineCore::GetInstance().GetPrimaryWorld();
        const uint64_t primaryWorldId = primaryWorld ? primaryWorld->GetWorldId() : 0;
        auto* renderingLoop = EngineCore::GetInstance().GetRenderingLoop();
        auto* systemManager = renderingLoop ? renderingLoop->GetSystemManager() : nullptr;
        const auto* animationSystem = systemManager ? systemManager->GetSystem<Engine::Renderer::AnimationSystem>() : nullptr;

        const bool withBoneNames = requestContext.params.value("withBoneNames", false);
        const int onlySkeletonId = requestContext.params.value("skeletonId", 0);

        json skeletons = json::array();
        for (uint32_t id = 1; id <= skeletonCount; ++id)
        {
            if (onlySkeletonId != 0 && static_cast<int>(id) != onlySkeletonId) continue;
            const auto* skeleton = store.Get(id);
            if (!skeleton) continue;

            json skeletonJson;
            skeletonJson["id"] = id;
            skeletonJson["boneCount"] = skeleton->BoneCount;
            skeletonJson["skinJointCount"] = skeleton->SkinJointCount;
            skeletonJson["jointNodeCount"] = static_cast<uint32_t>(skeleton->JointNodes.size());

            if (withBoneNames)
            {
                json names = json::array();
                for (const auto& boneName : skeleton->BoneNames)
                    names.push_back(boneName);
                skeletonJson["boneNames"] = std::move(names);

                // Compute per-bone world rest rotations (composing local
                // RestRotation from root). Returns {x, y, z, w} per bone.
                // Useful for retarget triage: comparing source vs target
                // bind world rotations identifies whether two rigs share
                // a canonical T-pose at bind.
                json worldRest = json::array();
                std::vector<Mathematics::Quaternion> worldRotations(
                    skeleton->BoneCount, Mathematics::Quaternion(0.0f, 0.0f, 0.0f, 0.0f));
                const bool hasTopologicalOrder = !skeleton->TopologicalOrder.empty()
                    && skeleton->TopologicalOrder.size() == skeleton->BoneCount;
                for (uint32_t index = 0; index < skeleton->BoneCount; ++index)
                {
                    const uint32_t boneIndex = hasTopologicalOrder ? skeleton->TopologicalOrder[index] : index;
                    const float* restRotation = (boneIndex * 4u + 3u < skeleton->RestRotation.size())
                        ? &skeleton->RestRotation[boneIndex * 4u] : nullptr;
                    Mathematics::Quaternion local = restRotation
                        ? Mathematics::Quaternion(restRotation[3], restRotation[0], restRotation[1], restRotation[2])
                        : Mathematics::Quaternion::Identity();
                    const int32 parent = (boneIndex < skeleton->Parent.size()) ? skeleton->Parent[boneIndex] : -1;
                    const Mathematics::Quaternion world = parent < 0
                        ? local : worldRotations[static_cast<size_t>(parent)] * local;
                    worldRotations[boneIndex] = world.Normalized();
                }
                for (uint32_t boneIndex = 0; boneIndex < skeleton->BoneCount; ++boneIndex)
                {
                    const auto& worldRotation = worldRotations[boneIndex].GetGLM();
                    worldRest.push_back({worldRotation.x, worldRotation.y, worldRotation.z, worldRotation.w});
                }
                skeletonJson["restWorld"] = std::move(worldRest);
            }

            skeletons.push_back(skeletonJson);
        }

        // Per-entity runtimes — required to debug retarget / skinning data
        // flow (atlas offset routing, palette content). Probes the first
        // non-identity transform to indicate whether the per-frame writer
        // (AnimationSystem CPU branch, HumanoidRetargetSystem, or GPU
        // compute) actually populated motion vs. left bind-pose identity.
        const size_t runtimeHighWaterMark = store.GetRuntimeHighWaterMark();
        json runtimes = json::array();
        for (size_t index = 0; index < runtimeHighWaterMark; ++index)
        {
            const uint32_t runtimeId = static_cast<uint32_t>(index + 1);
            const uint32_t skeletonId = store.GetRuntimeSkeletonId(runtimeId);
            if (skeletonId == 0) continue; // freed slot

            const auto* runtime = store.GetRuntime(runtimeId);
            if (!runtime) continue;

            json runtimeJson;
            runtimeJson["runtimeId"] = runtimeId;
            runtimeJson["skeletonId"] = skeletonId;
            // How many live SkeletonRef components name this runtime — one per
            // entity of the model instance. Deleting one sibling drops it by
            // one; the slot is freed only when the last reference goes.
            runtimeJson["refCount"] = store.GetRuntimeRefCount(runtimeId);
            runtimeJson["atlasPaletteOffsetBones"] = runtime->AtlasPaletteOffsetBones;
            runtimeJson["paletteMatrixCount"] = static_cast<uint32_t>(runtime->CompactSkinMatrices.size() / 16);
            const auto* selection = animationSystem ? animationSystem->GetBoneLODSelection(runtimeId) : nullptr;
            const bool selectionCurrent = selection &&
                selection->RuntimeGeneration == store.GetRuntimeGeneration(runtimeId) &&
                runtime->Source.WorldId == primaryWorldId && primaryWorld &&
                runtime->Source.WorldGeneration == primaryWorld->GetLifecycleResetGeneration();
            const auto* boneData = selectionCurrent ? selection->Data.get() : nullptr;
            const bool reduced = boneData && selection->Reduced;
            const auto* skeleton = store.Get(skeletonId);
            runtimeJson["boneLOD"] = json{
                {"selectionKnown", selectionCurrent},
                {"tier", reduced ? "reduced" : "full"},
                {"requestedBudget", boneData ? boneData->RequestedBoneBudget() : 0u},
                {"requiredBones", boneData ? boneData->RequiredBoneCount() : 0u},
                {"evaluatedBones", reduced ? boneData->ActualBoneCount() :
                    (skeleton ? static_cast<uint32>(skeleton->JointClosure.size()) : 0u)},
                {"error", boneData ? boneData->Error() : std::string{}},
                {"warning", boneData ? boneData->Warning() : std::string{}}};

            // Atlas owner: which subsystem wrote this runtime's atlas slot
            // this frame. The skin-matrix probe below reads the CPU mirror,
            // which is only updated by the CPU eval paths. When the GPU owns
            // the slot, the probe values reflect the last CPU eval (often
            // never), NOT what the renderer reads.
            const char* atlasOwner = "cpu"; // SkinningUploadSystem default
            if (retargetStore && retargetStore->IsRetargetGPUHandled(runtimeId))
                atlasOwner = "gpu-retarget";
            else if (gpuAnimation && gpuAnimation->IsGPUHandled(runtimeId))
                atlasOwner = "gpu-skinning";
            runtimeJson["atlasOwner"] = atlasOwner;
            runtimeJson["paletteProbeIsStale"] = (atlasOwner[0] == 'g'); // gpu-* owners

            // Probe for the first non-identity matrix and report its
            // translation magnitude + max diagonal deviation. This tells
            // the caller whether the palette is bind-pose-identity
            // (suggesting the writer never ran or produced no motion) or
            // contains real animated transforms.
            uint32_t firstNonIdentityIndex = UINT32_MAX;
            float maxTranslationMagnitude = 0.0f;
            float maxOffDiagonalMagnitude = 0.0f;
            const auto& matrices = runtime->CompactSkinMatrices;
            const size_t matrixCount = matrices.size() / 16;
            for (size_t matrixIndex = 0; matrixIndex < matrixCount; ++matrixIndex)
            {
                const float* values = &matrices[matrixIndex * 16];
                // Column-major: translation is at indices [12..14]; diagonal at [0],[5],[10].
                const float translationX = values[12], translationY = values[13], translationZ = values[14];
                const float translationMagnitude = std::sqrt(translationX*translationX + translationY*translationY + translationZ*translationZ);
                // Off-diagonal deviation captures any rotation away from identity.
                const float offDiagonalMagnitude = std::max({
                    std::fabs(values[1]), std::fabs(values[2]), std::fabs(values[4]),
                    std::fabs(values[6]), std::fabs(values[8]), std::fabs(values[9])
                });
                if (translationMagnitude > maxTranslationMagnitude) maxTranslationMagnitude = translationMagnitude;
                if (offDiagonalMagnitude > maxOffDiagonalMagnitude) maxOffDiagonalMagnitude = offDiagonalMagnitude;
                const bool isIdentityIsh =
                    translationMagnitude < 1e-5f &&
                    std::fabs(values[0] - 1.0f) < 1e-5f &&
                    std::fabs(values[5] - 1.0f) < 1e-5f &&
                    std::fabs(values[10] - 1.0f) < 1e-5f &&
                    offDiagonalMagnitude < 1e-5f;
                if (!isIdentityIsh && firstNonIdentityIndex == UINT32_MAX)
                    firstNonIdentityIndex = static_cast<uint32_t>(matrixIndex);
            }
            runtimeJson["firstNonIdentityJointIndex"] = firstNonIdentityIndex == UINT32_MAX
                ? -1
                : static_cast<int>(firstNonIdentityIndex);
            runtimeJson["maxTranslationMag"] = maxTranslationMagnitude;
            runtimeJson["maxRotationOffDiagonal"] = maxOffDiagonalMagnitude;
            runtimes.push_back(runtimeJson);
        }

        json result;
        result["skeletonCount"] = skeletonCount;
        result["primaryWorldId"] = primaryWorldId;
        result["skeletons"] = skeletons;
        result["runtimes"] = runtimes;
        result["runtimeHighWaterMark"] = static_cast<uint32_t>(runtimeHighWaterMark);
        return result; });
}
}
