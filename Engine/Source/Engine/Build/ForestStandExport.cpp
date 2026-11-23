#include "ForestStandExport.h"

#include "ContentCheckRegistry.h"

#include "Engine/Build/AssetCollector.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/FlatMap.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

// The entity that holds a field: "entities[e_stand].ForestStand.Archive" gives "e_stand", and
// "lid/ForestStand.Archive" gives "lid".
std::string_view LocatorEntity(std::string_view locator, std::string_view field)
{
    std::string_view owner = locator.substr(0, locator.size() - field.size());
    if (!owner.empty())
        owner.remove_suffix(1);
    constexpr std::string_view kEntities = "entities[";
    if (owner.starts_with(kEntities) && owner.ends_with(']'))
        owner = owner.substr(kEntities.size(), owner.size() - kEntities.size() - 1);
    return owner;
}

using ShippedArchive = std::pair<const AssetManifestEntry*, std::string>;

// What the stands of one scene reference, by edge locator: true when one edge of the locator ships.
struct StandReferences
{
    FlatMap<std::string, bool> Archives;
    FlatMap<std::string, bool> TreeModels;
    // Each archive that ships, with the locator of its first field, in edge order.
    std::vector<ShippedArchive> ShippedArchives;
};

StandReferences CollectStandReferences(std::span<const DepEdge> edges, const AssetManifest& manifest)
{
    StandReferences references;
    for (const DepEdge& edge : edges)
    {
        const bool archiveField = NamesField(edge.FieldLocator, kArchiveField);
        if (!archiveField && !NamesField(edge.FieldLocator, kTreeModelField))
            continue;
        const AssetManifestEntry* target = edge.Target.IsNull() ? nullptr : manifest.FindByGuid(edge.Target);
        auto& shipped = (archiveField ? references.Archives : references.TreeModels).GetOrInsert(edge.FieldLocator);
        shipped = shipped || target != nullptr;
        if (archiveField && target &&
            std::ranges::find(references.ShippedArchives, target, &ShippedArchive::first) == references.ShippedArchives.end())
            references.ShippedArchives.emplace_back(target, edge.FieldLocator);
    }
    return references;
}

// The start of a refusal: the scene or prefab and the entity that holds the field.
std::string DescribeStand(const std::string& fileName, std::string_view locator, std::string_view field)
{
    return fileName + ", entity '" + std::string(LocatorEntity(locator, field)) + "': the Forest Stand ";
}

void CheckScene(const AssetManifestEntry& scene, std::span<const DepEdge> edges, const AssetManifest& manifest,
                std::vector<std::string>& errors)
{
    const StandReferences references = CollectStandReferences(edges, manifest);
    const std::string fileName = (scene.type == AssetType::Prefab ? "Prefab '" : "Scene '") +
                                  PathToUtf8(AssetRelativePath(scene)) + "'";
    for (const auto& [locator, shipped] : references.Archives)
        if (!shipped)
            errors.push_back(DescribeStand(fileName, locator, kArchiveField) +
                             "archive does not ship. Set Archive to the .geforest file in the project that the "
                             "overview mesh was cooked from.");
    for (const auto& [locator, shipped] : references.TreeModels)
        if (!shipped)
            errors.push_back(DescribeStand(fileName, locator, kTreeModelField) +
                             "tree model does not ship. Set Tree Model to a model in the project with its impostor "
                             "setting on, or clear it.");
    for (const auto& [archive, locator] : references.ShippedArchives)
        if (std::string problem = DescribeArchiveProblem(*archive); !problem.empty())
            errors.push_back(DescribeStand(fileName, locator, kArchiveField) + "archive '" +
                             PathToUtf8(AssetRelativePath(*archive)) + "' " + problem);
}
// Only a file that a dependency walk read has edges. The fallbacks of CollectGameContent walk no
// file and ship every project asset, so the check cannot see a broken archive there.
void CheckForestStandContent(const AssetManifest& manifest, const AssetCollector& collector,
                             std::vector<std::string>& errors)
{
    for (const AssetManifestEntry& entry : manifest.entries)
        if (entry.type == AssetType::Scene || entry.type == AssetType::Prefab)
            if (const std::vector<DepEdge>* edges = collector.FindWalkedEdges(entry.guid))
                CheckScene(entry, *edges, manifest, errors);
}
} // namespace

void RegisterForestStandContentCheck()
{
    ContentCheckRegistry::Register(&CheckForestStandContent);
}
} // namespace GameEngine
