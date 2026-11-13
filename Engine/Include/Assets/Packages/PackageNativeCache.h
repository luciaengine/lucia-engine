#pragma once

#include <filesystem>
#include <memory>

namespace GameEngine::FileSystem
{
class ScopedFileLock;
}

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{
/// Pin a managed native cache entry, recording the original package-cache root that owns its scope.
/// Prune the inactive generations of the entry's own scope.
/// The returned lease must outlive every build and loaded module using the entry.
/// Unsupported platforms, unsafe paths and lock failures return null without deleting data.
std::unique_ptr<FileSystem::ScopedFileLock> AcquirePackageNativeCache(
    const std::filesystem::path& entry, const std::filesystem::path& cacheRoot);

/// Keep an entry owned by GlobalPackageCacheRoot pinned until process termination, including project switches.
/// Takes the lease before it returns, then queues one job on `maintenance` that prunes the entry's old
/// generations and, once per process, removes the orphaned scopes of the shared root. Pass a channel no frame waits on.
bool RetainPackageNativeCache(const std::filesystem::path& entry, JobSystem::JobChannel& maintenance);
}
