#include "ForestStandExport.h"

#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/FlatMap.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace GameEngine
{
namespace fs = std::filesystem;

namespace
{
constexpr std::string_view kArchiveField = "ForestStand.Archive";
constexpr std::string_view kTreeModelField = "ForestStand.TreeModel";

// True when a dependency edge locator ("entities[id].ForestStand.Archive", "lid/ForestStand.Archive") names the field.
bool NamesField(std::string_view locator, std::string_view field)
{
    if (!locator.ends_with(field))
        return false;
    if (locator.size() == field.size())
        return true;
    const char separator = locator[locator.size() - field.size() - 1];
    return separator == '.' || separator == '/';
}

// The asset-relative path of an entry: its output path after the first "Assets" folder.
fs::path AssetRelativePath(const AssetManifestEntry& entry)
{
    fs::path relative;
    bool inAssets = false;
    for (const auto& part : entry.outputPath)
    {
        if (inAssets)
            relative /= part;
        else if (part == "Assets")
            inAssets = true;
    }
    return relative;
}

// Empty when the archive resolves inside its asset folder and opens.
std::string DescribeArchiveProblem(const AssetManifestEntry& archive)
{
    const fs::path relative = AssetRelativePath(archive);
    if (archive.type != AssetType::ForestPlacementArchive)
        return "is not a .geforest file. Set Archive to the .geforest file that the overview mesh was cooked from.";

    fs::path root = archive.sourcePath;
    for (auto part = relative.begin(); part != relative.end(); ++part)
        root = root.parent_path();
    std::error_code error;
    const fs::path canonicalRoot = fs::canonical(root, error);
    const fs::path source = error ? fs::path{} : fs::canonical(archive.sourcePath, error);
    if (error)
        return "cannot be read: " + error.message() + ".";
    const fs::path inside = source.lexically_relative(canonicalRoot);
    if (inside.empty() || inside.has_root_path() ||
        std::ranges::any_of(inside, [](const fs::path& part) { return part == ".."; }))
        return "resolves outside its asset folder. Replace the link with the archive file.";

    PageStreaming::PackedPlacementStore store;
    if (std::string reason = store.Open(source); !reason.empty())
        return "is not valid: " + reason + ". Cook the archive again with ForestPlacementCook.";
    return {};
}

// Keeps the edges one parser call emits.
class CollectingDepEdgeSink final : public DepEdgeSink
{
public:
    void Emit(DepEdge edge) override { Edges.push_back(std::move(edge)); }
    std::vector<DepEdge> Edges;
};

// The dependency edges of a scene or prefab as its parser reads them from the file now.
std::vector<DepEdge> ReadSceneEdges(const AssetManifestEntry& scene, const AssetRegistry& registry,
                                    const ParserRegistry& parsers)
{
    AssetMetadata metadata;
    const auto parser = parsers.FindParser(scene.sourcePath);
    if (!parser || !registry.TryGetAssetMetadata(scene.guid, metadata))
        return {};
    metadata.Path = scene.sourcePath;
    CollectingDepEdgeSink sink;
    if (!parser->ExtractDependencies(scene.guid, metadata, sink))
        return {};
    for (DepEdge& edge : sink.Edges)
        if (edge.Target.IsNull() && !edge.TargetPath.empty())
            edge.Target = registry.ResolvePathTarget(edge.TargetPath);
    return std::move(sink.Edges);
}

// What the stands of one scene reference, by edge locator: true when one edge of the locator ships.
struct StandReferences
{
    FlatMap<std::string, bool> Archives;
    FlatMap<std::string, bool> TreeModels;
};

void CheckScene(const AssetManifestEntry& scene, const AssetManifest& manifest, const AssetRegistry& registry,
                const ParserRegistry& parsers, std::vector<std::string>& errors)
{
    StandReferences references;
    std::vector<const AssetManifestEntry*> archives;
    for (const DepEdge& edge : ReadSceneEdges(scene, registry, parsers))
    {
        const bool archiveField = NamesField(edge.FieldLocator, kArchiveField);
        if (!archiveField && !NamesField(edge.FieldLocator, kTreeModelField))
            continue;
        const AssetManifestEntry* target = edge.Target.IsNull() ? nullptr : manifest.FindByGuid(edge.Target);
        auto& shipped = (archiveField ? references.Archives : references.TreeModels).GetOrInsert(edge.FieldLocator);
        shipped = shipped || target != nullptr;
        if (archiveField && target && std::ranges::find(archives, target) == archives.end())
            archives.push_back(target);
    }

    const std::string sceneName = "Scene '" + PathToUtf8(AssetRelativePath(scene)) + "'";
    for (const auto& [locator, shipped] : references.Archives)
        if (!shipped)
            errors.push_back(sceneName + ": the ForestStand archive does not ship. Set Archive to the .geforest "
                                         "file in the project that the overview mesh was cooked from.");
    for (const auto& [locator, shipped] : references.TreeModels)
        if (!shipped)
            errors.push_back(sceneName + ": the ForestStand tree model does not ship. Set Tree Model to a model "
                                         "in the project with its impostor setting on, or clear it.");
    for (const AssetManifestEntry* archive : archives)
        if (std::string problem = DescribeArchiveProblem(*archive); !problem.empty())
            errors.push_back(sceneName + ": the ForestStand archive '" + PathToUtf8(AssetRelativePath(*archive)) +
                             "' " + problem);
}
} // namespace

std::vector<std::string> CheckForestStandContent(const AssetManifest& manifest, const AssetRegistry& registry,
                                                 const ParserRegistry& parsers)
{
    std::vector<std::string> errors;
    for (const AssetManifestEntry& entry : manifest.entries)
        if (entry.type == AssetType::Scene || entry.type == AssetType::Prefab)
            CheckScene(entry, manifest, registry, parsers, errors);
    return errors;
}
} // namespace GameEngine
