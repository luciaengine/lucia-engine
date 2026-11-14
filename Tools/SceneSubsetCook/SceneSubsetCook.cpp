// Derive inspectable runtime data from the canonical scene parser. No ECS,
// renderer, asset registry, authored-source edits or project-specific heuristics.
#include "Scene/SceneIO.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <regex>

int main(int argc, char** argv)
{
    using namespace GameEngine;
    namespace fs = std::filesystem;
    try
    {
        fs::path source, output, selection, groupFile;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            fs::path* destination = argument == "--scene" ? &source : argument == "--output" ? &output :
                argument == "--selection" ? &selection : argument == "--groups" ? &groupFile : nullptr;
            if (!destination || i + 1 >= argc || !destination->empty())
                throw std::runtime_error("Usage: SceneSubsetCook --scene FILE --output NEW_DIRECTORY [--selection IDS_JSON | --groups GROUPS_JSON]");
            *destination = argv[++i];
        }
        if (source.empty() || output.empty())
            throw std::runtime_error("Scene and new output directory are required");
        if (!selection.empty() && !groupFile.empty())
            throw std::runtime_error("Use selection OR groups, not both");
        source = fs::canonical(source);
        output = fs::absolute(output).lexically_normal();
        if (fs::exists(output) || fs::is_symlink(output) || !fs::is_directory(output.parent_path()))
            throw std::runtime_error("Output must be a new directory inside an existing parent; nothing overwritten");
        // Reject linked output ancestry, including dangling links. This is not
        // an adversarial concurrent-filesystem sandbox; keep the parent owned.
        for (auto path = output.parent_path(); !path.empty(); path = path.parent_path())
        {
            if (fs::is_symlink(path))
                throw std::runtime_error("Output ancestry must not contain symlinks");
            if (path == path.root_path())
                break;
        }
        auto prepared = Scene::PrepareSceneFromFile(source);
        if (!prepared)
            throw std::runtime_error(Scene::GetLastSceneIOError().message);
        if (!groupFile.empty())
        {
            std::ifstream input(groupFile);
            if (!input) throw std::runtime_error("Could not open group file");
            nlohmann::json requested; input >> requested;
            if (!requested.is_object() || requested.empty())
                throw std::runtime_error("Groups must be a nonempty JSON object of name to entity-id arrays");
            std::vector<std::vector<std::string>> groups;
            std::vector<std::string> names;
            for (const auto& [name, ids] : requested.items())
            {
                // Lowercase portable names; no path traversal, NTFS streams,
                // case collisions, device names or dot/space aliases.
                if (!std::regex_match(name, std::regex("[a-z][a-z0-9_-]{0,79}")) ||
                    std::regex_match(name, std::regex("con|prn|aux|nul|com[0-9]|lpt[0-9]")))
                    throw std::runtime_error("Unsafe group name: " + name);
                names.push_back(name);
                groups.push_back(ids.get<std::vector<std::string>>());
            }
            const auto selected = Scene::SelectPreparedSceneGroups(*prepared, groups);
            if (!selected) throw std::runtime_error(Scene::GetLastSceneIOError().message);
            // Prepare every serialized document before claiming the output root.
            std::vector<std::string> documents;
            for (const auto& scene : *selected)
            {
                auto document = Scene::SerializePreparedScene(*scene);
                if (!document) throw std::runtime_error(Scene::GetLastSceneIOError().message);
                documents.push_back(std::move(*document));
            }
            if (!fs::create_directory(output)) throw std::runtime_error("Could not exclusively create output directory");
            nlohmann::json result = nlohmann::json::object();
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                const auto filename = names[i] + ".scene";
                std::ofstream file(output/filename, std::ios::binary);
                file << documents[i]; file.close();
                if (!file) throw std::runtime_error("Failed writing group; discard partial generation");
                const auto roundtrip = Scene::PrepareSceneFromFile(output/filename);
                if (!roundtrip || Scene::SerializePreparedScene(*roundtrip) != std::optional<std::string>(documents[i]))
                    throw std::runtime_error("Derived group failed canonical roundtrip: " + filename);
                const auto info = Scene::DescribePreparedScene(*(*selected)[i]);
                if (!info) throw std::runtime_error(Scene::GetLastSceneIOError().message);
                std::vector<std::string> ids;
                for (const auto& entity : *info) ids.push_back(entity.Id);
                result[names[i]] = {{"file", filename}, {"entity_ids", ids}, {"bytes", documents[i].size()}};
            }
            std::ofstream marker(output/"complete.json", std::ios::binary);
            marker << nlohmann::json{{"format", 1}, {"groups", result}, {"source", source.generic_string()}}.dump(2) << '\n';
            marker.close();
            if (!marker) throw std::runtime_error("Failed writing group-completion marker");
            std::cout << "Prepared scene groups: " << names.size() << '\n';
            return 0;
        }
        nlohmann::json roots = nullptr;
        if (!selection.empty())
        {
            std::ifstream input(selection);
            if (!input)
                throw std::runtime_error("Could not open selection file");
            input >> roots;
            if (!roots.is_array())
                throw std::runtime_error("Selection must be a JSON array of exact entity ids");
            const auto ids = roots.get<std::vector<std::string>>();
            prepared = Scene::SelectPreparedSceneEntities(*prepared, ids);
            if (!prepared)
                throw std::runtime_error(Scene::GetLastSceneIOError().message);
        }
        const auto catalog = Scene::DescribePreparedScene(*prepared);
        if (!catalog)
            throw std::runtime_error(Scene::GetLastSceneIOError().message);
        nlohmann::json entities = nlohmann::json::array();
        for (const auto& entity : *catalog)
            entities.push_back({{"id", entity.Id}, {"name", entity.Name},
                {"parent", entity.Parent ? nlohmann::json(*entity.Parent) : nlohmann::json(nullptr)},
                {"components", entity.Components}, {"dependency_ids", entity.DependencyIds}});
        nlohmann::json report{{"format", 1}, {"source", source.generic_string()},
            {"selection_roots", roots}, {"entities", std::move(entities)},
            {"dependency_policy", "parents-and-conservative-canonical-values"},
            {"semantic_dependencies_inferred", false}};
        std::optional<std::string> encoded;
        if (!selection.empty())
        {
            encoded = Scene::SerializePreparedScene(*prepared);
            if (!encoded)
                throw std::runtime_error(Scene::GetLastSceneIOError().message);
        }
        // Only after all preparation succeeds do we claim a new output root.
        if (!fs::create_directory(output))
            throw std::runtime_error("Could not exclusively create output directory");
        const auto write = [&](const char* name, const std::string& bytes)
        {
            std::ofstream file(output/name, std::ios::binary);
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.close();
            if (!file)
                throw std::runtime_error("Failed writing derived output; discard partial generation");
        };
        if (encoded)
            write("scene.scene", *encoded);
        write("catalog.json", report.dump(2) + '\n');
        // This is a tool-completion marker, NOT a signed build receipt or a
        // runtime acceptance claim. Build integration must bind hashes and pins.
        write("complete.json", nlohmann::json{{"format", 1}, {"entities", catalog->size()},
              {"subset_emitted", encoded.has_value()}}.dump(2) + '\n');
        std::cout << "Prepared scene entities: " << catalog->size() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
