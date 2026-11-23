#include "Engine/Build/ContentCheckRegistry.h"

#include "ForestStandExport.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace GameEngine
{
namespace
{

struct Registry
{
    std::mutex Mutex;
    std::vector<ContentCheck> Checks;
};

Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

// Registers the engine's own checks once, before the first run, so no init order matters.
void EnsureBuiltInsRegistered()
{
    static std::once_flag once;
    std::call_once(once, &RegisterForestStandContentCheck);
}

} // namespace

void ContentCheckRegistry::Register(ContentCheck check)
{
    if (check.Name.empty() || !check.Run)
        return;
    Registry& registry = GetRegistry();
    const std::scoped_lock lock(registry.Mutex);
    const auto existing = std::ranges::find(registry.Checks, check.Name, &ContentCheck::Name);
    if (existing != registry.Checks.end())
        *existing = std::move(check);
    else
        registry.Checks.push_back(std::move(check));
}

std::vector<std::string> ContentCheckRegistry::RunAll(const AssetManifest& manifest, const AssetCollector& collector)
{
    EnsureBuiltInsRegistered();
    std::vector<ContentCheck> checks;
    {
        Registry& registry = GetRegistry();
        const std::scoped_lock lock(registry.Mutex);
        checks = registry.Checks;
    }
    std::vector<std::string> errors;
    for (const ContentCheck& check : checks)
        check.Run(manifest, collector, errors);
    return errors;
}

} // namespace GameEngine
