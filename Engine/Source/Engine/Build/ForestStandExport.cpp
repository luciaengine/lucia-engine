#include "ForestStandExport.h"

#include "ContentCheckRegistry.h"

#include "AssetCore/SharedFileRead.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Rendering/Forest/ForestSplatCells.h"
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
constexpr std::string_view kSplatsField = "ForestStand.Splats";

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

// Empty when the splat bake is a valid .geforestsplats file.
std::string DescribeSplatsProblem(const AssetManifestEntry& splats)
{
    if (splats.type != AssetType::ForestSplats)
        return "is not a .geforestsplats file. Set Splats to the file that ForestPlacementCook --splats baked.";
    ByteBuffer bytes;
    std::string error = "the file cannot be read";
    if (ReadFileBytesShared(splats.sourcePath, bytes, Engine::Renderer::ForestSplatCells::kMaxFileBytes) &&
        Engine::Renderer::ParseForestSplatCells(bytes, error))
        return {};
    return "is not valid: " + error + ". Bake the tree model again with ForestPlacementCook --splats.";
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

using ShippedFile = std::pair<const AssetManifestEntry*, std::string>;

// What the stands of one scene reference, by edge locator: true when one edge of the locator ships.
struct StandReferences
{
    FlatMap<std::string, bool> Archives;
    FlatMap<std::string, bool> TreeModels;
    FlatMap<std::string, bool> Splats;
    // Each archive and splat bake that ships, with the locator of its first field, in edge order.
    std::vector<ShippedFile> ShippedArchives;
    std::vector<ShippedFile> ShippedSplats;
};

StandReferences CollectStandReferences(std::span<const DepEdge> edges, const AssetManifest& manifest)
{
    StandReferences references;
    for (const DepEdge& edge : edges)
    {
        const bool archiveField = NamesField(edge.FieldLocator, kArchiveField);
        const bool splatsField = NamesField(edge.FieldLocator, kSplatsField);
        if (!archiveField && !splatsField && !NamesField(edge.FieldLocator, kTreeModelField))
            continue;
        const AssetManifestEntry* target = edge.Target.IsNull() ? nullptr : manifest.FindByGuid(edge.Target);
        auto& fieldReferences = archiveField ? references.Archives : (splatsField ? references.Splats : references.TreeModels);
        auto& shipped = fieldReferences.GetOrInsert(edge.FieldLocator);
        shipped = shipped || target != nullptr;
        if (!target || (!archiveField && !splatsField))
            continue;
        auto& files = archiveField ? references.ShippedArchives : references.ShippedSplats;
        if (std::ranges::find(files, target, &ShippedFile::first) == files.end())
            files.emplace_back(target, edge.FieldLocator);
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
    for (const auto& [locator, shipped] : references.Splats)
        if (!shipped)
            errors.push_back(DescribeStand(fileName, locator, kSplatsField) +
                             "splat bake does not ship. Set Splats to a .geforestsplats file in the project, or "
                             "clear it.");
    for (const auto& [splats, locator] : references.ShippedSplats)
        if (std::string problem = DescribeSplatsProblem(*splats); !problem.empty())
            errors.push_back(DescribeStand(fileName, locator, kSplatsField) + "splat bake '" +
                             PathToUtf8(AssetRelativePath(*splats)) + "' " + problem);
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
