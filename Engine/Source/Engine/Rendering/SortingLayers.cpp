#include "Engine/Rendering/SortingLayers.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <set>
#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
namespace
{
std::mutex s_Mutex;
SortingLayers s_Layers = SortingLayers::Defaults();
std::atomic<uint64> s_Revision{1};

int32 ReadLayerInteger(const nlohmann::json& value)
{
    if (!value.is_number_integer())
        throw std::runtime_error("layer ID and priority must be integers");
    if (value.is_number_unsigned())
    {
        const auto number = value.get<uint64>();
        if (number > static_cast<uint64>(std::numeric_limits<int32>::max()))
            throw std::runtime_error("layer ID or priority is outside the signed 32-bit range");
        return static_cast<int32>(number);
    }
    const auto number = value.get<int64>();
    if (number < std::numeric_limits<int32>::min() || number > std::numeric_limits<int32>::max())
        throw std::runtime_error("layer ID or priority is outside the signed 32-bit range");
    return static_cast<int32>(number);
}
}
SortingLayers SortingLayers::Defaults()
{
    return {{{-30, "Background", -30}, {-20, "Distant", -20}, {-10, "Scenery", -10},
             {0, "Default", 0}, {10, "Gameplay", 10}, {20, "Effects", 20},
             {30, "Foreground", 30}, {40, "Overlay", 40}}};
}
SortingLayers SortingLayers::Current()
{
    std::lock_guard lock(s_Mutex);
    return s_Layers;
}
uint64 SortingLayers::Revision() { return s_Revision.load(std::memory_order_acquire); }
bool SortingLayers::Validate(std::string& error) const
{
    std::set<int32> ids;
    std::set<int32> orders;
    std::set<std::string> names;
    for (const auto& layer : Layers)
    {
        if (layer.Name.empty() || layer.Name.size() > 80 ||
            layer.Name.find_first_not_of(" \t\r\n") == std::string::npos ||
            !ids.insert(layer.Id).second || !orders.insert(layer.Order).second ||
            !names.insert(layer.Name).second)
        {
            error = "Sorting layers need unique IDs, priorities and nonempty names of at most 80 characters.";
            return false;
        }
    }
    if (!ids.contains(0))
    {
        error = "Keep the default sorting layer with ID 0.";
        return false;
    }
    error.clear();
    return true;
}
bool SortingLayers::SetCurrent(const SortingLayers& layers, std::string& error)
{
    if (!layers.Validate(error))
        return false;
    std::lock_guard lock(s_Mutex);
    if (s_Layers.Layers != layers.Layers)
    {
        s_Layers = layers;
        s_Revision.fetch_add(1, std::memory_order_release);
    }
    return true;
}
int32 SortingLayers::Priority(int32 id) const
{
    for (const auto& layer : Layers)
        if (layer.Id == id)
            return layer.Order;
    // A removed layer uses Default until its entities are reassigned.
    for (const auto& layer : Layers)
        if (layer.Id == 0)
            return layer.Order;
    return 0;
}
SortingLayers SortingLayers::FromJson(const nlohmann::json& root)
{
    if (!root.contains("sortingLayers"))
        return Defaults();
    SortingLayers result;
    try
    {
        const auto& array = root.at("sortingLayers");
        if (!array.is_array())
            throw std::runtime_error("sortingLayers must be an array");
        for (const auto& entry : array)
            result.Layers.push_back({ReadLayerInteger(entry.at("id")), entry.at("name").get<std::string>(),
                                     ReadLayerInteger(entry.at("order"))});
        std::string error;
        if (!result.Validate(error))
            throw std::runtime_error(error);
        return result;
    }
    catch (const std::exception& error)
    {
        Logger::Log::Warning("Sorting layers use defaults: {}. Fix sortingLayers in project settings.", error.what());
        return Defaults();
    }
}
void SortingLayers::WriteToJson(nlohmann::json& root) const
{
    auto& array = root["sortingLayers"] = nlohmann::json::array();
    for (const auto& layer : Layers)
        array.push_back({{"id", layer.Id}, {"name", layer.Name}, {"order", layer.Order}});
}
SortingLayers SortingLayers::Load(const std::filesystem::path& projectRoot)
{
    std::ifstream input(projectRoot / ".Editor" / "ProjectSettings.json");
    if (!input)
        return Defaults();
    try { return FromJson(nlohmann::json::parse(input)); }
    catch (const std::exception& error)
    {
        Logger::Log::Warning("Cannot read sorting layers: {}", error.what());
        return Defaults();
    }
}
}
