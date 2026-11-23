#pragma once

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetCollector;
struct AssetManifest;

/// A check of the content that an export ships. It adds one sentence per problem to `errors`, and
/// each sentence names the file and the fix.
using ContentCheck =
    std::function<void(const AssetManifest& manifest, const AssetCollector& collector, std::vector<std::string>& errors)>;

/// The content checks that AssetCollector::CollectGameContent runs on every export target. The
/// engine's own checks register on first use. Thread-safe.
class ContentCheckRegistry
{
public:
    /// Adds a check.
    static void Register(ContentCheck check);
    /// The errors of every check, in registration order.
    static std::vector<std::string> RunAll(const AssetManifest& manifest, const AssetCollector& collector);
};

} // namespace GameEngine
