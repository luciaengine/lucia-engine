#include "DebugHandlerHelpers.h"

#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"

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

server.RegisterHandler("get_skeleton_store_state", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        const uint32_t count = store.GetCount();

        // Resolve the optional GPU stores so we can label each runtime's
        // atlas owner. The skin-matrix probe below operates on the runtime's
        // CompactSkinMatrices (CPU cache), which is intentionally not
        // refreshed when GPU owns the slot — without an owner tag, the
        // identity-looking probe values can be misread as a rendering bug.
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        const auto* gpuAnim = rs ? &rs->GetGPUAnimationDataStore() : nullptr;
        const auto* retargetFeat = rs ? rs->GetFeature<Engine::Renderer::RetargetRenderFeature>() : nullptr;
        const auto* retargetStore = (retargetFeat && retargetFeat->IsInitialized())
            ? &retargetFeat->GetDataStore() : nullptr;

        auto* primaryWorld = EngineCore::GetInstance().GetPrimaryWorld();
        const uint64_t primaryWorldId = primaryWorld ? primaryWorld->GetWorldId() : 0;
        auto* loop = EngineCore::GetInstance().GetRenderingLoop();
        auto* systems = loop ? loop->GetSystemManager() : nullptr;
        const auto* animation = systems ? systems->GetSystem<Engine::Renderer::AnimationSystem>() : nullptr;

        const bool withBoneNames = ctx.params.value("withBoneNames", false);
        const int onlySkeletonId = ctx.params.value("skeletonId", 0);

        json skeletons = json::array();
        for (uint32_t id = 1; id <= count; ++id)
        {
            if (onlySkeletonId != 0 && static_cast<int>(id) != onlySkeletonId) continue;
            const auto* skel = store.Get(id);
            if (!skel) continue;

            json skelJson;
            skelJson["id"] = id;
            skelJson["boneCount"] = skel->BoneCount;
            skelJson["skinJointCount"] = skel->SkinJointCount;
            skelJson["jointNodeCount"] = static_cast<uint32_t>(skel->JointNodes.size());

            if (withBoneNames)
            {
                json names = json::array();
                for (const auto& n : skel->BoneNames)
                    names.push_back(n);
                skelJson["boneNames"] = std::move(names);

                // Compute per-bone world rest rotations (composing local
                // RestRotation from root). Returns {x, y, z, w} per bone.
                // Useful for retarget triage: comparing source vs target
                // bind world rotations identifies whether two rigs share
                // a canonical T-pose at bind.
                json worldRest = json::array();
                std::vector<std::array<float, 4>> worldQuats(skel->BoneCount);
                const bool useTopo = !skel->TopologicalOrder.empty()
                    && skel->TopologicalOrder.size() == skel->BoneCount;
                for (uint32_t i = 0; i < skel->BoneCount; ++i)
                {
                    const uint32_t b = useTopo ? skel->TopologicalOrder[i] : i;
                    const float* rp = (b * 4u + 3u < skel->RestRotation.size())
                        ? &skel->RestRotation[b * 4u] : nullptr;
                    glm::quat local = rp
                        ? glm::quat(rp[3], rp[0], rp[1], rp[2])
                        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                    const int32 parent = (b < skel->Parent.size()) ? skel->Parent[b] : -1;
                    glm::quat world;
                    if (parent < 0) world = local;
                    else
                    {
                        const auto& p = worldQuats[static_cast<size_t>(parent)];
                        glm::quat parentW(p[3], p[0], p[1], p[2]);
                        world = parentW * local;
                    }
                    world = glm::normalize(world);
                    worldQuats[b] = {world.x, world.y, world.z, world.w};
                }
                for (uint32_t b = 0; b < skel->BoneCount; ++b)
                {
                    const auto& w = worldQuats[b];
                    worldRest.push_back({w[0], w[1], w[2], w[3]});
                }
                skelJson["restWorld"] = std::move(worldRest);
            }

            skeletons.push_back(skelJson);
        }

        // Per-entity runtimes — required to debug retarget / skinning data
        // flow (atlas offset routing, palette content). Probes the first
        // non-identity transform to indicate whether the per-frame writer
        // (AnimationSystem CPU branch, HumanoidRetargetSystem, or GPU
        // compute) actually populated motion vs. left bind-pose identity.
        const size_t runtimeHighWater = store.GetRuntimeHighWaterMark();
        json runtimes = json::array();
        for (size_t i = 0; i < runtimeHighWater; ++i)
        {
            const uint32_t rtId = static_cast<uint32_t>(i + 1);
            const uint32_t skelId = store.GetRuntimeSkeletonId(rtId);
            if (skelId == 0) continue; // freed slot

            const auto* runtime = store.GetRuntime(rtId);
            if (!runtime) continue;

            json rtJson;
            rtJson["runtimeId"] = rtId;
            rtJson["skeletonId"] = skelId;
            // How many live SkeletonRef components name this runtime — one per
            // entity of the model instance. Deleting one sibling drops it by
            // one; the slot is freed only when the last reference goes.
            rtJson["refCount"] = store.GetRuntimeRefCount(rtId);
            rtJson["atlasPaletteOffsetBones"] = runtime->AtlasPaletteOffsetBones;
            rtJson["paletteMatrixCount"] = static_cast<uint32_t>(runtime->CompactSkinMatrices.size() / 16);
            const auto* selection = animation ? animation->GetBoneLODSelection(rtId) : nullptr;
            const bool selectionCurrent = selection &&
                selection->RuntimeGeneration == store.GetRuntimeGeneration(rtId) &&
                runtime->Source.WorldId == primaryWorldId && primaryWorld &&
                runtime->Source.WorldGeneration == primaryWorld->GetLifecycleResetGeneration();
            const auto* boneData = selectionCurrent ? selection->Data.get() : nullptr;
            const bool reduced = boneData && selection->Reduced;
            const auto* skeleton = store.Get(skelId);
            rtJson["boneLOD"] = json{
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
            if (retargetStore && retargetStore->IsRetargetGPUHandled(rtId))
                atlasOwner = "gpu-retarget";
            else if (gpuAnim && gpuAnim->IsGPUHandled(rtId))
                atlasOwner = "gpu-skinning";
            rtJson["atlasOwner"] = atlasOwner;
            rtJson["paletteProbeIsStale"] = (atlasOwner[0] == 'g'); // gpu-* owners

            // Probe for the first non-identity matrix and report its
            // translation magnitude + max diagonal deviation. This tells
            // the caller whether the palette is bind-pose-identity
            // (suggesting the writer never ran or produced no motion) or
            // contains real animated transforms.
            uint32_t firstNonIdentityIdx = UINT32_MAX;
            float maxTranslationMag = 0.0f;
            float maxOffDiagonalAbs = 0.0f;
            const auto& m = runtime->CompactSkinMatrices;
            const size_t mats = m.size() / 16;
            for (size_t j = 0; j < mats; ++j)
            {
                const float* p = &m[j * 16];
                // Column-major: translation is at indices [12..14]; diagonal at [0],[5],[10].
                const float tx = p[12], ty = p[13], tz = p[14];
                const float tmag = std::sqrt(tx*tx + ty*ty + tz*tz);
                // Off-diagonal deviation captures any rotation away from identity.
                const float od = std::max({
                    std::fabs(p[1]), std::fabs(p[2]), std::fabs(p[4]),
                    std::fabs(p[6]), std::fabs(p[8]), std::fabs(p[9])
                });
                if (tmag > maxTranslationMag) maxTranslationMag = tmag;
                if (od > maxOffDiagonalAbs) maxOffDiagonalAbs = od;
                const bool isIdentityIsh =
                    tmag < 1e-5f &&
                    std::fabs(p[0] - 1.0f) < 1e-5f &&
                    std::fabs(p[5] - 1.0f) < 1e-5f &&
                    std::fabs(p[10] - 1.0f) < 1e-5f &&
                    od < 1e-5f;
                if (!isIdentityIsh && firstNonIdentityIdx == UINT32_MAX)
                    firstNonIdentityIdx = static_cast<uint32_t>(j);
            }
            rtJson["firstNonIdentityJointIndex"] = firstNonIdentityIdx == UINT32_MAX
                ? -1
                : static_cast<int>(firstNonIdentityIdx);
            rtJson["maxTranslationMag"] = maxTranslationMag;
            rtJson["maxRotationOffDiagonal"] = maxOffDiagonalAbs;
            runtimes.push_back(rtJson);
        }

        json result;
        result["skeletonCount"] = count;
        result["primaryWorldId"] = primaryWorldId;
        result["skeletons"] = skeletons;
        result["runtimes"] = runtimes;
        result["runtimeHighWaterMark"] = static_cast<uint32_t>(runtimeHighWater);
        return result; });
}
}
