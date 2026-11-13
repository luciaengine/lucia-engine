#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/AppIconGenerator.h"
#include "Engine/Build/PackageBuildStaging.h"
#include "Engine/Build/StagedAssetCook.h"
#include "Engine/Build/TexturePackageCook.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"
#include "NativeScripting/NativeSourceTree.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
namespace fs = std::filesystem;
void AppendModule(nlohmann::json& modules, const fs::path& root, const fs::path& package, const fs::path& generated,
                                        const std::vector<std::string>& defines)
{
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& source : NativeScripting::CollectWatchedNativeSources(root, package))
    {
        const auto ext = ToLowerAscii(source.extension().string());
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c") sources.push_back(source.string());
    }
    for (const char* name : {"UserComponentReflection.gen.cpp", "UserSystemRegistration.gen.cpp"})
        if (fs::is_regular_file(generated / name)) sources.push_back((generated / name).string());
    if (!sources.empty()) modules.push_back({{"sources", sources}, {"includeRoot", root.string()}, {"defines", defines}});
}

}

bool BuildPipeline::PrepareMobileContent(const BuildSettings& settings, std::filesystem::path& contentRoot,
                                         std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path project = fs::weakly_canonical(settings.projectRoot);
    const fs::path output = fs::weakly_canonical(settings.outputDirectory);
    if (settings.projectRoot.empty() || settings.outputDirectory.empty() || output == project
        || output == output.root_path() || !RelativePathUnderRoot(project, output).empty())
    {
        error = "Choose a mobile output folder separate from the project root and its parents.";
        return false;
    }
    for (const char* name : {"Assets", "Packages", "Platforms", "Managed", "UserScripts", ".Editor", ".git"})
    {
        const auto authored = fs::weakly_canonical(project / name);
        if (output == authored || !RelativePathUnderRoot(output, authored).empty())
        {
            error = "Choose a mobile output folder outside authored project folders.";
            return false;
        }
    }
    if (FailIfCancelled() || !WaitForNativeModuleBuilds())
    {
        error = m_Progress.statusMessage;
        return false;
    }
    ResolvePackagesForBuild(settings);
    if (ProjectHasCSharpSources(project / "Assets"))
    {
        error = "Mobile C# gameplay requires ahead-of-time compilation, which is not supported by this native Player build.";
        return false;
    }
    nlohmann::json modules = nlohmann::json::array();
    AppendModule(modules, project / "Assets", {}, project / ".Cache/NativeScripts/build", {});
    for (const auto& module : m_PackageRuntimeModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::Cpp)
        {
            error = "Mobile C# package gameplay requires ahead-of-time compilation: " + module.AssemblyName;
            return false;
        }
        AppendModule(modules, module.RootDir, module.PackageRootDir, module.CacheDir / "NativeScripts/build", module.Defines);
    }
    fs::create_directories(output);
    for (const auto* name : {"mobile-native-sources.json", ".mobile-icons"})
    {
        if (fs::is_symlink(output / name))
        {
            error = "Remove the symbolic link at the mobile staging path: " + (output / name).string();
            return false;
        }
    }
    std::ofstream nativeList(output / "mobile-native-sources.json");
    nativeList << nlohmann::json{{"modules", modules}}.dump(2);
    if (!nativeList) { error = "Cannot write mobile native source list."; return false; }
    contentRoot = output / ".content";
    if (fs::is_symlink(contentRoot))
    {
        error = "Remove the symbolic link at the mobile content staging path.";
        return false;
    }
    fs::remove_all(contentRoot);
    fs::create_directories(contentRoot);
    const auto icons = GenerateApplicationIcons(settings, output / ".mobile-icons");
    if (!icons.error.empty()) { error = icons.error; return false; }
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    AssetManifest manifest = CollectAssets(settings);
    if (!m_Progress.errors.empty()) { error = m_Progress.errors.back(); return false; }
    BuildContext context{settings, contentRoot, manifest};
    if (!RunPackageAssetStep(context) || !CopyAssets(contentRoot, manifest)
        || !CopyEngineShaders(contentRoot)
        || !WriteGameConfig(contentRoot, settings) || !StageAssetManifest(contentRoot, manifest)
        || !StagePackagesIndex(contentRoot, manifest, m_PackageResolution, m_PackageRuntimeModules, m_Progress.errors))
    {
        error = m_Progress.errors.empty() ? "Failed to prepare mobile content; see the build log." : m_Progress.errors.back();
        return false;
    }
    TexturePackageCookStats stats;
    if (!CookStagedContent(contentRoot, manifest, assets.GetRegistry(), assets.GetParserRegistry(),
        assets.GetTextureCookWorkers(), &EngineCore::GetInstance().GetJobSystem(), [this] { return IsCancelled(); }, stats, error)) return false;
    CookMeshLODs(contentRoot, manifest);
    StageTerrainBakes(contentRoot, settings, manifest);
    CopyThirdPartyNotices(contentRoot, settings);
    return !FailIfCancelled();
}
}
