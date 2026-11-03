#include "Service/LightmapBakeService.h"
#include "Service/LightmapBakeSelection.h"
#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "Components/Rendering/Light.h"
#include "Components/Name.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Baker/ShadowLightmapBaker.h"
#include <glm/gtc/type_ptr.hpp>
#include <stb_image.h>
#include <stb_image_write.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <set>

namespace GameEngine::Editor
{
namespace
{
using json = nlohmann::json;
using namespace Engine::Renderer;
namespace fs = std::filesystem;

struct CpuTexture
{
    int Width = 0, Height = 0;
    bool ClampV = false;
    std::vector<glm::vec3> Pixels;
    std::array<float,8> Transform{1,0,0,0,0,1,0,0};
    glm::vec3 Sample(glm::vec2 uv) const
    {
        uv = {uv.x*Transform[0]+uv.y*Transform[1]+Transform[2], uv.x*Transform[4]+uv.y*Transform[5]+Transform[6]};
        const glm::vec2 wrapped(glm::fract(uv.x), ClampV ? std::clamp(uv.y,0.0f,1.0f) : glm::fract(uv.y));
        const auto p = wrapped*glm::vec2(Width,Height)-0.5f;
        const int x = int(std::floor(p.x)), y = int(std::floor(p.y));
        auto texel = [&](int ix,int iy) { return Pixels[size_t(ClampV ? std::clamp(iy,0,Height-1) : (iy%Height+Height)%Height)*Width+(ix%Width+Width)%Width]; };
        return glm::mix(glm::mix(texel(x,y),texel(x+1,y),glm::fract(p.x)),
                        glm::mix(texel(x,y+1),texel(x+1,y+1),glm::fract(p.x)),glm::fract(p.y));
    }
};
float Scalar(const MaterialDocument& doc, const char* name, float fallback)
{
    auto i = doc.properties.find(name);
    if (i != doc.properties.end()) if (auto v = std::get_if<float>(&i->second)) return *v;
    return fallback;
}
glm::vec3 Color(const MaterialDocument& doc, const char* name, glm::vec3 fallback)
{
    auto i = doc.properties.find(name);
    if (i != doc.properties.end())
        if (auto v = std::get_if<std::vector<float>>(&i->second); v && v->size()>=3) return {(*v)[0],(*v)[1],(*v)[2]};
    return fallback;
}
fs::path TexturePath(AssetManager& assets, const MaterialDocument& doc, const fs::path& materialPath, const std::string& name)
{
    if (auto i = doc.textures.find(name); i != doc.textures.end() && !i->second.empty())
    {
        AssetMetadata meta;
        const GUID guid(i->second);
        if (!guid.IsNull() && assets.GetRegistry().TryGetAssetMetadata(guid,meta)) return meta.Path;
        const auto path = assets.ResolveAssetPathFromReference(i->second,materialPath);
        if (fs::is_regular_file(path)) return path;
    }
    if (auto i = doc.texturePaths.find(name); i != doc.texturePaths.end() && !i->second.empty())
        return assets.ResolveAssetPathFromReference(i->second,materialPath);
    return {};
}
std::shared_ptr<CpuTexture> LoadTexture(AssetManager& assets, MaterialDocument& doc, const fs::path& materialPath,
                                       const std::string& name, bool linear)
{
    const auto path = TexturePath(assets,doc,materialPath,name);
    if (path.empty())
    {
        if ((doc.textures.contains(name) && !doc.textures.at(name).empty()) ||
            (doc.texturePaths.contains(name) && !doc.texturePaths.at(name).empty()))
            throw std::runtime_error("Could not resolve bake texture: " + name);
        return {};
    }
    auto image = std::make_shared<CpuTexture>();
    int channels;
    if (!stbi_info(path.string().c_str(),&image->Width,&image->Height,&channels) ||
        image->Width <= 0 || image->Height <= 0 || uint64_t(image->Width)*image->Height > 16777216)
        throw std::runtime_error("Unsupported or oversized bake texture: " + path.string());
    image->Pixels.resize(size_t(image->Width)*image->Height);
    if (stbi_is_hdr(path.string().c_str()))
    {
        float* data = stbi_loadf(path.string().c_str(),&image->Width,&image->Height,&channels,3);
        if (!data) throw std::runtime_error("Could not decode HDR bake texture: " + path.string());
        for (size_t i=0; i<image->Pixels.size(); ++i) image->Pixels[i] = {data[i*3],data[i*3+1],data[i*3+2]};
        stbi_image_free(data);
    }
    else
    {
        unsigned char* data = stbi_load(path.string().c_str(),&image->Width,&image->Height,&channels,3);
        if (!data) throw std::runtime_error("Could not decode bake texture: " + path.string());
        std::string colorSpace;
        if (assets.GetRegistry().TryGetMetaValue(path,kTextureColorSpaceMetaKey,colorSpace))
        {
            TextureColorSpace parsed;
            if (ParseTextureColorSpaceMeta(colorSpace,parsed)) linear = parsed == TextureColorSpace::Linear;
        }
        for (size_t i=0; i<image->Pixels.size(); ++i)
            for (int c=0; c<3; ++c)
            {
                const float v = data[i*3+c]/255.0f;
                image->Pixels[i][c] = linear ? v : (v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f));
            }
        stbi_image_free(data);
    }
    if (auto i=doc.textureTransforms.find(name); i!=doc.textureTransforms.end()) image->Transform=i->second;
    return image;
}
struct Snapshot
{
    std::string Name;
    ECS::EntityHandle Entity;
    Components::MeshRenderer Renderer;
    Components::WorldTransform Transform;
    Mesh Geometry;
    ShadowLightmapMesh BakeMesh;
    MaterialDocument Material;
    std::vector<std::pair<fs::path,fs::file_time_type>> SourceStamps;
    bool Receiver = false;
};
struct LightSnapshot
{
    ECS::EntityHandle Entity;
    Components::Light Light;
    Components::WorldTransform Transform;
};
struct BakeJob
{
    std::string Id;
    fs::path Root, ProjectRoot;
    ECS::World* World = nullptr;
    uint64_t SceneVersion = 0;
    std::vector<Snapshot> Meshes;
    std::vector<LightSnapshot> Lights;
    std::vector<std::pair<fs::path,fs::file_time_type>> EnvironmentStamps;
    std::atomic<bool> Cancel{false};
    std::atomic<uint32_t> Completed{0};
    std::atomic<float> Progress{0};
    std::atomic<bool> UsingGpu{false};
    uint32_t ReceiverCount = 0;
    std::future<json> Future;
    json Result;
    bool Apply = true;
};

void ReleaseBakeInputs(BakeJob& job)
{
    // Keep the result for the panel, but release copied geometry and decoded
    // source images before another job starts snapshotting the scene.
    job.Meshes.clear();
    job.Lights.clear();
    job.EnvironmentStamps.clear();
}

// Keep the job alive while joining; clearing its future breaks the async
// callable's ownership of the job even if the client never polls completion.
struct BakeSession
{
    std::shared_ptr<BakeJob> Job;
    ~BakeSession()
    {
        if (Job && Job->Future.valid())
        {
            Job->Cancel = true;
            Job->Future.wait();
            Job->Future = {};
            std::error_code ec;
            fs::remove_all(Job->Root,ec);
        }
    }
};

Snapshot SnapshotMesh(ECS::World& world, uint32_t id, bool receiver, AssetManager& assets, bool inspectOnly=false)
{
    Snapshot result;
    result.Entity = ECS::EntityHandle(id); result.Receiver = receiver;
    if (!world.IsValid(result.Entity)) throw std::runtime_error("Invalid mesh entity " + std::to_string(id));
    const auto* name=world.GetComponent<Components::Name>(result.Entity);
    result.Name=name && name->value[0]?name->value:"Unnamed mesh";
    auto* renderer = world.GetComponent<Components::MeshRenderer>(result.Entity);
    auto* transform = world.GetComponent<Components::WorldTransform>(result.Entity);
    if (!renderer || !transform || world.HasComponent<ECS::ComponentDisabled<Components::MeshRenderer>>(result.Entity) || world.GetComponent<ECS::Disabled>(result.Entity) ||
        world.GetComponent<Components::SkinnedMeshRenderer>(result.Entity))
        throw std::runtime_error("Bake selection must contain enabled static mesh renderers.");
    result.Renderer = *renderer; result.Transform = *transform;
    auto load = [&](const GUID& guid) {
        auto asset = assets.GetAsset(guid);
        if (!asset || !asset->IsLoaded()) asset = assets.LoadAssetAsync(guid).get();
        return asset;
    };
    const auto modelAsset = load(renderer->modelAssetGuid.ToGuid());
    if (!modelAsset || modelAsset->GetType()!=AssetType::Model || !modelAsset->IsLoaded())
        throw std::runtime_error("Could not load model for entity " + std::to_string(id));
    const auto& model = *static_cast<ModelAsset*>(modelAsset.get());
    const auto index = ResolveSubmeshIndex(model.GetGUID(),model,*renderer);
    if (index>=model.GetMeshCount()) throw std::runtime_error("Invalid receiver submesh.");
    result.SourceStamps.emplace_back(model.GetPath(),fs::last_write_time(model.GetPath()));
    const auto& geometry = model.GetMesh(index);
    if (!inspectOnly) result.Geometry = geometry;
    if (geometry.HasMorphTargets()) throw std::runtime_error("Morph-target geometry is not supported by static lightmap baking.");
    result.BakeMesh.LocalToWorld = glm::make_mat4(transform->matrix) * glm::make_mat4(geometry.SourceNodeTransform);
    if (receiver && (!geometry.HasTexCoords1() || geometry.HasOwnVertexLODs()))
        throw std::runtime_error("Receiver needs a second UV set for lightmaps, shared by all of its detail levels.");
    fs::path materialPath = model.GetPath();
    const auto materialAsset = renderer->materialAssetGuid.IsNull() ? SharedPtr<Asset>{} : load(renderer->materialAssetGuid.ToGuid());
    if (materialAsset && materialAsset->IsLoaded() && materialAsset->GetType()==AssetType::Material)
    {
        result.Material = static_cast<MaterialAsset*>(materialAsset.get())->GetDocument();
        materialPath = materialAsset->GetPath();
    }
    else if (geometry.MaterialIndex < model.GetMaterials().size() &&
        (renderer->materialAssetGuid.IsNull() || renderer->materialAssetGuid.ToGuid()==ModelMaterialBridge::DeriveMaterialGuid(model.GetGUID(),geometry.MaterialIndex)))
        result.Material = ModelMaterialBridge::Convert(model.GetGUID(),geometry.MaterialIndex,model.GetMaterial(geometry.MaterialIndex)).document;
    else throw std::runtime_error("Could not resolve material for entity " + std::to_string(id));
    if (materialPath!=model.GetPath()) result.SourceStamps.emplace_back(materialPath,fs::last_write_time(materialPath));
    auto& doc = result.Material;
    result.BakeMesh.UseVertexColor = !doc.ignoreVertexColor;
    const auto surfaceName = fs::path(doc.surfaceShader).filename().string();
    if (doc.alphaMode != MaterialAlphaMode::Opaque || !doc.vertexModifier.empty() || !doc.vertexModifierGuid.empty() ||
        !doc.surfaceGraph.empty() || !doc.surfaceGraphGuid.empty() ||
        (surfaceName!="standard_pbr.glsl" && surfaceName!="shadow_lightmap_pbr.glsl") ||
        Scalar(doc,"hexTiling",0)>0 || (Scalar(doc,"transmissionWeight",0)>0 && doc.properties.contains("enableTransmission") &&
        std::get_if<bool>(&doc.properties.at("enableTransmission")) && std::get<bool>(doc.properties.at("enableTransmission"))))
        throw std::runtime_error("Baking currently supports opaque Standard PBR without vertex deformation or hex tiling.");
    if (inspectOnly) return result;
    const auto albedo = LoadTexture(assets,doc,materialPath,"albedoMap",false);
    const auto emission = LoadTexture(assets,doc,materialPath,"emissiveMap",false);
    const auto metalRough = LoadTexture(assets,doc,materialPath,"metallicRoughnessMap",true);
    const auto baseColor = Color(doc,"baseColor",glm::vec3(1));
    const auto emissionColor = Color(doc,"emissive",glm::vec3(0));
    const float emissionScale = Scalar(doc,"emissionLuminance",203)/203;
    const float metallic = Scalar(doc,"metallic",0);
    if (!albedo && !emission && !metalRough)
        result.BakeMesh.Surface = {baseColor*(1-std::clamp(metallic,0.0f,1.0f)),emissionColor*emissionScale};
    else result.BakeMesh.SampleSurface = [=](glm::vec2 uv) {
        LightmapSurface s;
        const float metal = metallic*(metalRough ? metalRough->Sample(uv).b : 1);
        s.Albedo = baseColor*(albedo ? albedo->Sample(uv) : glm::vec3(1))*(1-std::clamp(metal,0.0f,1.0f));
        s.Emission = (emissionColor+(emission ? emission->Sample(uv) : glm::vec3(0)))*emissionScale;
        return s;
    };
    // Resolve existing texture references before moving the material to the bake folder.
    for (auto& [name,value] : doc.textures)
    {
        if (name=="shadowLightmapMap" || name=="indirectLightmapMap") continue;
        const auto path = TexturePath(assets,doc,materialPath,name);
        if (!path.empty())
        {
            result.SourceStamps.emplace_back(path,fs::last_write_time(path));
            value = assets.GetRegistry().GetOrCreateAssetGUID(path).ToString();
            doc.texturePaths[name] = fs::relative(path,assets.GetAssetRoot()).generic_string();
        }
    }
    return result;
}

json RunBake(const std::shared_ptr<BakeJob>& job, ShadowLightmapBakeOptions options, float fadeStart, float fadeEnd, float mode)
{
    try
    {
        std::vector<ShadowLightmapMesh> occluders;
        for (auto& mesh : job->Meshes)
        {
            mesh.BakeMesh.Geometry = &mesh.Geometry;
            // All selected static surfaces participate in GI. Cast-shadow-off
            // meshes are rejected by the entry point to keep the two paths consistent.
            occluders.push_back(mesh.BakeMesh);
        }
        options.Continue = [job] { return !job->Cancel.load(); };
        options.AccelerationChanged=[job](bool gpu) { job->UsingGpu=gpu; };
        // All receivers share the same immutable caster snapshot. Build one GPU
        // scene per job, and retain it only for the lifetime of this worker.
        auto createTracer=options.CreateRayTracer;
        LightmapRayBatch sceneTracer;
        bool attempted=false, gpuFailed=false;
        if (createTracer) options.CreateRayTracer=[&](std::span<const float> positions) -> LightmapRayBatch {
            if (!attempted) { attempted=true; sceneTracer=createTracer(positions); }
            if (!sceneTracer || gpuFailed) return {};
            return [&](std::span<const LightmapRay> rays,std::span<LightmapRayHit> hits) {
                if (sceneTracer(rays,hits)) return true;
                gpuFailed=true;
                return false;
            };
        };
        uint64_t totalGpuRays=0, totalRays=0;
        json rows = json::array();
        fs::create_directories(job->Root);
        for (auto& mesh : job->Meshes)
        {
            if (!mesh.Receiver) continue;
            const uint32_t completed=job->Completed.load();
            options.Progress=[job,completed](float value) {
                // Leave the final portion for writing the assets for this mesh.
                job->Progress=(completed+0.98f*value)/job->ReceiverCount;
            };
            ShadowLightmapBakeResult result;
            std::string error;
            if (!BakeShadowLightmap(mesh.BakeMesh,occluders,options,result,error)) throw std::runtime_error(mesh.Name+": "+error);
            if (job->Cancel) throw std::runtime_error("Lightmap bake cancelled.");
            const auto name = std::to_string(mesh.Entity.id);
            const auto shadow = job->Root/(name+"_shadow.png");
            const auto indirect = job->Root/(name+"_indirect.hdr");
            const auto material = job->Root/(name+".material");
            if (!stbi_write_png((shadow.string()+".tmp").c_str(),result.Width,result.Height,1,result.Visibility.data(),result.Width))
                throw std::runtime_error("Could not write baked shadowmap.");
            if (!result.IndirectRGB.empty() && !stbi_write_hdr((indirect.string()+".tmp").c_str(),result.Width,result.Height,3,result.IndirectRGB.data()))
                throw std::runtime_error("Could not write HDR indirect lightmap.");
            fs::rename(shadow.string()+".tmp",shadow);
            if (!result.IndirectRGB.empty()) fs::rename(indirect.string()+".tmp",indirect);
            auto doc = mesh.Material;
            doc.materialName += " (Baked GI)";
            doc.surfaceShader = "Surfaces/shadow_lightmap_pbr.glsl"; doc.surfaceShaderGuid.clear();
            doc.surfaceGraph.clear(); doc.surfaceGraphGuid.clear();
            if (std::find(doc.keywords.begin(), doc.keywords.end(), "CUSTOM_LIGHTING") == doc.keywords.end())
                doc.keywords.push_back("CUSTOM_LIGHTING");
            doc.textureFilter = MaterialTextureFilter::Bilinear;
            doc.textures.erase("roughnessMap"); doc.textures.erase("metallicMap");
            doc.texturePaths.erase("roughnessMap"); doc.texturePaths.erase("metallicMap");
            doc.textures["shadowLightmapMap"] = AssetRegistry::DeriveGuidForSourcePath("project",fs::relative(shadow,job->ProjectRoot).generic_string()).ToString();
            doc.texturePaths["shadowLightmapMap"] = fs::relative(shadow,job->ProjectRoot).generic_string();
            doc.textures["indirectLightmapMap"] = result.IndirectRGB.empty() ? "" : AssetRegistry::DeriveGuidForSourcePath("project",fs::relative(indirect,job->ProjectRoot).generic_string()).ToString();
            doc.texturePaths["indirectLightmapMap"] = result.IndirectRGB.empty() ? "" : fs::relative(indirect,job->ProjectRoot).generic_string();
            // Use the shared generic user lanes; baking owns no core property names.
            // Scalar aliases must not race the vectors when documents are applied.
            for (const char* key : {"user0", "user1", "user2", "user3", "user4", "user5", "user6", "user7",
                                    "shadowLightmapMode", "shadowLightmapFadeStart", "shadowLightmapFadeEnd",
                                    "indirectLightmapEnabled", "shadowLightmapDirection"})
                doc.properties.erase(key);
            doc.properties["userVec0"] = std::vector<float>{mode, fadeStart, fadeEnd,
                result.IndirectRGB.empty() ? 0.0f : 1.0f};
            const auto direction = glm::normalize(options.DirectionToLight);
            doc.properties["userVec1"] = std::vector<float>{direction.x, direction.y, direction.z, 0.0f};
            std::ofstream out(material.string()+".tmp");
            out << SerializeMaterialDocument(doc).dump(2); out.close();
            if (!out) throw std::runtime_error("Could not write baked material.");
            fs::rename(material.string()+".tmp",material);
            totalGpuRays+=result.GpuTracedRays; totalRays+=result.TracedRays;
            rows.push_back({{"entityId",mesh.Entity.id},{"material",material.string()},{"shadow",shadow.string()},
                {"indirect",result.IndirectRGB.empty()?"":indirect.string()},{"coveredTexels",result.CoveredTexels},{"tracedRays",result.TracedRays},{"gpuTracedRays",result.GpuTracedRays}});
            ++job->Completed;
            job->Progress=float(job->Completed.load())/job->ReceiverCount;
        }
        if (job->Cancel) throw std::runtime_error("Lightmap bake cancelled.");
        json manifest{{"status","complete"},{"jobId",job->Id},{"receivers",rows},{"indirectSamples",options.IndirectSamples},
            {"acceleration",totalGpuRays==0?"cpu":totalGpuRays==totalRays?"gpu_ray_query":"gpu_and_cpu"},
            {"gpuTracedRays",totalGpuRays},{"tracedRays",totalRays},
            {"bounces",options.IndirectBounces},{"width",options.Width},{"height",options.Height},{"denoised",options.Denoise && options.IndirectSamples>0}};
        std::ofstream out(job->Root/"bake.json"); out << manifest.dump(2); out.close();
        if (!out) throw std::runtime_error("Could not write bake manifest.");
        return manifest;
    }
    catch (const std::exception& e)
    {
        // Output directory is unique to this job; never remove pre-existing assets.
        std::error_code ec; fs::remove_all(job->Root,ec);
        if (job->Cancel) return {{"status","cancelled"},{"jobId",job->Id}};
        return {{"status","failed"},{"error",e.what()},{"jobId",job->Id}};
    }
}
}

std::vector<uint32_t> CollectSceneLightmapReceivers(std::vector<std::string>& skipped)
{
    skipped.clear();
    std::vector<uint32_t> candidates, receivers;
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    if (!world) return receivers;
    auto& assets=EngineCore::GetInstance().GetAssetManager();
    world->Query<ECS::Read<Components::MeshRenderer>>().Each([&](ECS::EntityHandle entity,const Components::MeshRenderer& mesh) {
        if (!world->HasComponent<ECS::ComponentDisabled<Components::MeshRenderer>>(entity) && mesh.castShadows && !world->GetComponent<ECS::Disabled>(entity))
            candidates.push_back(entity.id);
    });
    std::sort(candidates.begin(),candidates.end());
    for (const auto id:candidates)
    {
        try
        {
            SnapshotMesh(*world,id,true,assets,true);
            receivers.push_back(id);
        }
        catch (const std::exception& e)
        {
            const auto* name=world->GetComponent<Components::Name>(ECS::EntityHandle(id));
            skipped.push_back(std::string(name && name->value[0]?name->value:"Unnamed mesh")+": "+e.what());
        }
    }
    return receivers;
}

struct LightmapBakeService::Impl : BakeSession {};

LightmapBakeService::LightmapBakeService(EditorChangeNotifications* notifications, std::function<bool()> canBake)
    : m_Impl(std::make_unique<Impl>()), m_Notifications(notifications), m_CanBake(std::move(canBake)) {}
LightmapBakeService::~LightmapBakeService() = default;

json LightmapBakeService::Start(const json& p)
{
    try
    {
        if (m_CanBake && !m_CanBake()) return {{"error","Stop playback before baking lighting."}};
        if (m_Impl->Job && m_Impl->Job->Future.valid()) return {{"error","A bake is active; query or cancel it first."}};
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world) return {{"error","No scene is open."}};
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        const auto receivers = p.at("receivers").get<std::vector<uint32_t>>();
        const auto occluders = p.at("occluders").get<std::vector<uint32_t>>();
        if (receivers.empty() || receivers.size()>4096 || occluders.size()>4096) return {{"error","Select 1..4096 receivers and at most 4096 static occluders."}};
        auto job = std::make_shared<BakeJob>();
        job->SceneVersion = m_SceneVersion;
        job->Id = GUID::Generate().ToString(); job->World = world; job->ProjectRoot = assets.GetAssetRoot();
        if (job->ProjectRoot.empty() || !job->ProjectRoot.is_absolute() || !fs::is_directory(job->ProjectRoot))
            return {{"error","No project asset directory."}};
        job->Root = job->ProjectRoot/"Lightmaps"/job->Id;
        // New bake assets belong to this project. Reject redirected folders
        // and pre-existing output paths before the worker owns its directory.
        const auto relative = fs::weakly_canonical(job->Root).lexically_relative(fs::weakly_canonical(job->ProjectRoot));
        if (relative.empty() || *relative.begin()==".." || fs::exists(job->Root))
            return {{"error","Bake output must be a new directory inside project assets."}};
        job->Apply = p.value("apply",true);
        std::set<uint32_t> receiverSet(receivers.begin(),receivers.end()), all(occluders.begin(),occluders.end());
        all.insert(receivers.begin(),receivers.end()); job->Meshes.reserve(all.size());
        for (auto id : all)
        {
            try
            {
                auto snapshot = SnapshotMesh(*world,id,receiverSet.contains(id),assets);
                if (!snapshot.Renderer.castShadows) throw std::runtime_error("Bake geometry must cast shadows; exclude disabled casters.");
                job->Meshes.push_back(std::move(snapshot));
            }
            catch (const std::exception& e)
            {
                const auto* name=world->GetComponent<Components::Name>(ECS::EntityHandle(id));
                throw std::runtime_error(std::string(name && name->value[0]?name->value:"Selected mesh")+": "+e.what());
            }
        }
        job->ReceiverCount=static_cast<uint32_t>(receiverSet.size());
        ShadowLightmapBakeOptions options;
        options.Width = options.Height = p.value("resolution",1024u);
        if (!options.Width || options.Width>kMaxLightmapResolution) return {{"error","Resolution must be between 1 and 8192 pixels."}};
        options.IndirectSamples = p.value("giSamples",256u); options.IndirectBounces = p.value("bounces",2u);
        options.Denoise = p.value("denoise",true);
        if (p.value("useGpu",true)) options.CreateRayTracer=CreateGpuLightmapRayTracer;
        options.RaysPerSample = p.value("shadowSamples",32u); options.Bias = p.value("bias",0.001f);
        options.Padding = p.value("padding",4u);
        const auto environment = p.value("environmentRadiance",std::vector<float>{0,0,0});
        if (environment.size()!=3) return {{"error","environmentRadiance must have three linear RGB values."}};
        options.EnvironmentRadiance = {environment[0],environment[1],environment[2]};
        if (p.contains("environmentTexture"))
        {
            MaterialDocument environmentDoc;
            environmentDoc.textures["environment"] = p.at("environmentTexture").get<std::string>();
            auto image = LoadTexture(assets,environmentDoc,job->ProjectRoot/"lightmap.material","environment",false);
            if (!image) return {{"error","Could not load environmentTexture."}};
            const auto environmentPath = TexturePath(assets,environmentDoc,job->ProjectRoot/"lightmap.material","environment");
            job->EnvironmentStamps.emplace_back(environmentPath,fs::last_write_time(environmentPath));
            image->ClampV = true;
            const float rotation = p.value("environmentRotationDegrees",0.0f)*(3.14159265359f/180.0f);
            const float strength = p.value("environmentIntensity",1.0f);
            if (!std::isfinite(rotation) || !std::isfinite(strength) || strength<0) return {{"error","Invalid environment rotation/intensity."}};
            options.SampleEnvironment = [image,rotation,strength](glm::vec3 d) {
                const float c=std::cos(rotation),s=std::sin(rotation);
                d={c*d.x-s*d.z,d.y,s*d.x+c*d.z};
                const glm::vec2 uv(std::atan2(d.z,d.x)/(2*3.14159265359f)+0.5f,std::acos(std::clamp(d.y,-1.0f,1.0f))/3.14159265359f);
                return image->Sample(uv)*strength;
            };
        }
        const ECS::EntityHandle sun(p.at("lightEntity").get<uint32_t>());
        if (!world->IsValid(sun)) return {{"error","Invalid directional light entity."}};
        if (auto* services=EngineCore::GetInstance().GetRenderServices())
        {
            const auto* primary=SelectPrimaryDirectional(services->GetWorldLights(world->GetWorldId()));
            if (!primary || primary->SortId!=sun.id)
                return {{"error","Choose the scene's primary directional light for the shadowmap."}};
        }
        auto* light = world->GetComponent<Components::Light>(sun);
        auto* transform = world->GetComponent<Components::WorldTransform>(sun);
        if (!light || !transform || world->HasComponent<ECS::ComponentDisabled<Components::Light>>(sun) || !light->CastsLight || world->GetComponent<ECS::Disabled>(sun) || light->Type!=Components::LightType::Directional)
            return {{"error","lightEntity must be the active primary directional light."}};
        job->Lights.push_back({sun,*light,*transform});
        options.DirectionToLight = -glm::normalize(glm::vec3(transform->matrix[8],transform->matrix[9],transform->matrix[10]));
        options.AngularRadiusRadians = light->ShadowAngularDiameter * (3.14159265359f/360.0f);
        float color[3], intensity; Components::ResolveLightColorIntensity(*light,color,intensity);
        options.LightRadiance = glm::vec3(color[0],color[1],color[2])*intensity;
        world->Query<ECS::Read<Components::Light>,ECS::Read<Components::WorldTransform>,ECS::Optional<ECS::Disabled>>()
            .Each([&](ECS::EntityHandle entity,const Components::Light& l,const Components::WorldTransform& t,const ECS::Disabled* disabled) {
                if (entity==sun || disabled || world->HasComponent<ECS::ComponentDisabled<Components::Light>>(entity) || !l.CastsLight || l.Type==Components::LightType::Ambient) return;
                if (l.Type!=Components::LightType::Directional && l.Type!=Components::LightType::Point && l.Type!=Components::LightType::Spot)
                    throw std::runtime_error("Area/volume light baking is not supported; use emissive geometry.");
                if (l.Type!=Components::LightType::Directional && l.Falloff!=Components::LightFalloff::PhysicalInverseSquare)
                    throw std::runtime_error("Baked punctual bounce lights require physical inverse-square falloff.");
                job->Lights.push_back({entity,l,t});
                LightmapBounceLight b;
                b.Kind = l.Type==Components::LightType::Directional ? LightmapBounceLight::Type::Directional :
                    l.Type==Components::LightType::Spot ? LightmapBounceLight::Type::Spot : LightmapBounceLight::Type::Point;
                b.Position = {t.matrix[12],t.matrix[13],t.matrix[14]}; b.Direction={t.matrix[8],t.matrix[9],t.matrix[10]};
                Components::ResolveLightColorIntensity(l,color,intensity); b.Radiance=glm::vec3(color[0],color[1],color[2])*intensity;
                b.Range=l.Range; b.InnerAngle=l.InnerAngle; b.OuterAngle=l.OuterAngle; options.BounceLights.push_back(b);
            });
        const float fadeStart=p.value("fadeStart",40.0f), fadeEnd=p.value("fadeEnd",50.0f);
        const std::string modeName=p.value("shadowMode",std::string("Overlay"));
        const float mode=modeName=="Replace"?1:modeName=="Overlay"?2:modeName=="Only"?3:0;
        if (!mode || !std::isfinite(fadeStart) || !std::isfinite(fadeEnd) || fadeStart<0 || fadeEnd<=fadeStart)
            return {{"error","Use Replace, Overlay or Only and a finite increasing fade range within the live shadow distance."}};
        job->Future=std::async(std::launch::async,[job,options,fadeStart,fadeEnd,mode] { return RunBake(job,options,fadeStart,fadeEnd,mode); });
        m_Impl->Job=job;
        return {{"status","baking"},{"jobId",job->Id},{"receivers",job->ReceiverCount}};
    }
    catch (const std::exception& e) { return {{"error",e.what()}}; }
}

json LightmapBakeService::Poll()
{
    auto job=m_Impl->Job;
    if (!job) return {{"status","idle"}};
    if (!job->Future.valid()) return job->Result;
    if (m_CanBake && !m_CanBake()) job->Cancel=true;
    if (job->Future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
        return {{"status","baking"},{"jobId",job->Id},{"completedReceivers",job->Completed.load()},
            {"totalReceivers",job->ReceiverCount},{"progress",job->Progress.load()},{"cancelling",job->Cancel.load()},
            {"acceleration",job->UsingGpu.load()?"gpu_ray_query":"cpu"}};
    job->Result=job->Future.get();
    // A click can arrive while the worker is writing its final assets. Honor
    // cancellation before importing or assigning even when tracing has ended.
    if (job->Cancel && job->Result["status"]=="complete")
    {
        std::error_code ec; fs::remove_all(job->Root,ec);
        job->Result={{"status","cancelled"},{"jobId",job->Id}};
    }
    if (job->Result["status"]!="complete")
    {
        ReleaseBakeInputs(*job);
        return job->Result;
    }
    auto& assets=EngineCore::GetInstance().GetAssetManager();
    auto* world=EngineCore::GetInstance().GetPrimaryWorld();
    if (assets.GetAssetRoot()!=job->ProjectRoot || world!=job->World || job->SceneVersion!=m_SceneVersion ||
        (m_CanBake && !m_CanBake()))
    {
        job->Result["applied"]=false; job->Result["reason"]="Scene/project changed during bake; outputs saved without assignment.";
        ReleaseBakeInputs(*job);
        return job->Result;
    }
    bool unchanged=true;
    for (const auto& [path,stamp] : job->EnvironmentStamps)
    {
        std::error_code ec;
        if (fs::last_write_time(path,ec)!=stamp || ec) unchanged=false;
    }
    for (const auto& s : job->Meshes)
    {
        auto* mr=world->GetComponent<Components::MeshRenderer>(s.Entity);
        auto* transform=world->GetComponent<Components::WorldTransform>(s.Entity);
        if (!mr || !transform || mr->modelAssetGuid!=s.Renderer.modelAssetGuid || mr->meshId!=s.Renderer.meshId ||
            mr->materialAssetGuid!=s.Renderer.materialAssetGuid || mr->MeshNameId!=s.Renderer.MeshNameId ||
            world->HasComponent<ECS::ComponentDisabled<Components::MeshRenderer>>(s.Entity) || mr->castShadows!=s.Renderer.castShadows ||
            world->GetComponent<ECS::Disabled>(s.Entity) || std::memcmp(transform->matrix,s.Transform.matrix,sizeof(s.Transform.matrix))) unchanged=false;
        for (const auto& [path,stamp] : s.SourceStamps)
        {
            std::error_code ec;
            if (fs::last_write_time(path,ec)!=stamp || ec) unchanged=false;
        }
    }
    for (const auto& s : job->Lights)
    {
        const auto* light=world->GetComponent<Components::Light>(s.Entity);
        const auto* transform=world->GetComponent<Components::WorldTransform>(s.Entity);
        if (!light || !transform || world->GetComponent<ECS::Disabled>(s.Entity) || std::memcmp(light,&s.Light,sizeof(s.Light)) ||
            std::memcmp(transform->matrix,s.Transform.matrix,sizeof(s.Transform.matrix))) unchanged=false;
    }
    size_t activeLightCount=0;
    world->Query<ECS::Read<Components::Light>,ECS::Read<Components::WorldTransform>,ECS::Optional<ECS::Disabled>>()
        .Each([&](ECS::EntityHandle entity,const Components::Light& light,const Components::WorldTransform&,const ECS::Disabled* disabled) {
            if (!disabled && !world->HasComponent<ECS::ComponentDisabled<Components::Light>>(entity) && light.CastsLight && light.Type!=Components::LightType::Ambient) ++activeLightCount;
        });
    if (activeLightCount!=job->Lights.size()) unchanged=false;
    bool registered=true;
    for (auto& row : job->Result["receivers"])
    {
        const fs::path shadow=row["shadow"].get<std::string>();
        registered &= assets.GetRegistry().RegisterAsset(shadow);
        registered &= assets.GetRegistry().SetMetaValue(shadow,kTextureColorSpaceMetaKey,"linear");
        const auto indirect=row["indirect"].get<std::string>();
        if (!indirect.empty()) registered &= assets.GetRegistry().RegisterAsset(indirect);
        registered &= assets.GetRegistry().RegisterAsset(row["material"].get<std::string>());
    }
    if (job->Apply && unchanged && registered)
    {
        for (auto& row : job->Result["receivers"])
        {
            ECS::EntityHandle entity(row["entityId"].get<uint32_t>());
            auto mr=*world->GetComponent<Components::MeshRenderer>(entity);
            const auto guid=assets.GetRegistry().GetAssetGUID(row["material"].get<std::string>());
            mr.materialAssetGuid.Set(guid);
            CommitComponentUpdate(world,entity,m_Notifications,mr);
            row["materialGuid"]=guid.ToString();
        }
        job->Result["applied"]=true;
    }
    else
    {
        job->Result["applied"]=false;
        job->Result["reason"]=!registered?"Asset registration failed.":!unchanged?"Geometry, materials, or lights changed during bake.":"Assignment disabled by request.";
    }
    ReleaseBakeInputs(*job);
    return job->Result;
}

void LightmapBakeService::Cancel()
{
    if (m_Impl->Job) m_Impl->Job->Cancel=true;
}

void LightmapBakeService::InvalidateScene()
{
    ++m_SceneVersion;
    Cancel();
    if (m_Impl->Job && !m_Impl->Job->Future.valid()) m_Impl->Job.reset();
}

void LightmapBakeService::Shutdown()
{
    InvalidateScene();
    m_Impl=std::make_unique<Impl>(); // cancel and join before editor teardown
}
}
