#pragma once

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetCollector;
struct AssetManifest;

/// A check of the content that an export ships. AssetCollector::CollectGameContent runs every
/// registered check on every export target and refuses the export when a check adds an error.
struct ContentCheck
{
    /// The name that a later registration replaces the check by.
    std::string Name;
    /// Adds one sentence per problem to `errors`. Each sentence names the file and the fix.
    std::function<void(const AssetManifest& manifest, const AssetCollector& collector, std::vector<std::string>& errors)>
        Run;
};

/// The content checks of the engine and its modules. The engine's own checks register on first use.
/// Thread-safe.
class ContentCheckRegistry
{
public:
    /// Adds a check, or replaces the check of the same name.
    static void Register(ContentCheck check);
    /// The errors of every check, in registration order.
    static std::vector<std::string> RunAll(const AssetManifest& manifest, const AssetCollector& collector);
};

} // namespace GameEngine
