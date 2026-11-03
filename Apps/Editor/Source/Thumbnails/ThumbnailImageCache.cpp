#include "Thumbnails/ThumbnailImageCache.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"

#include "AssetCore/SharedFileRead.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <limits>

#include <algorithm>
#include <system_error>

namespace GameEngine
{
namespace Editor
{
std::filesystem::file_time_type WriteTimeOrDefault(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

ThumbnailImage DecodeImage(const std::filesystem::path& path)
{
    ThumbnailImage image;
    if (path.empty())
        return image;

    Vector<uint8> bytes;
    if (!ReadFileBytesShared(path, bytes))
        return image;
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return image;
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                          &width, &height, &channels, 4);
    if (!pixels)
        return image;
    image.Width = width;
    image.Height = height;
    image.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;

}

bool WritePng(const std::filesystem::path& path, const ThumbnailImage& image)
{
    if (image.IsEmpty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), image.Width, image.Height, 4,
                          image.Pixels.data(), image.Width * 4) != 0;
}

void HashCombine(std::size_t& seed, std::size_t value)
{
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

void HashPathAndWriteTime(std::size_t& seed, const std::filesystem::path& path)
{
    HashCombine(seed, std::hash<std::string>{}(path.generic_string()));
    HashCombine(seed, std::hash<int64>{}(
                          WriteTimeOrDefault(path).time_since_epoch().count()));
}

void RemoveStaleCacheFiles(const std::filesystem::path& cacheRoot,
                           const std::string& identityPrefix,
                           const std::filesystem::path& keepPath)
{
    if (cacheRoot.empty() || identityPrefix.empty() || keepPath.empty())
        return;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cacheRoot, ec))
    {
        if (ec)
            return;
        const std::filesystem::path& candidate = entry.path();
        if (candidate == keepPath)
            continue;
        if (candidate.filename().string().rfind(identityPrefix, 0) != 0)
            continue;
        std::error_code removeEc;
        std::filesystem::remove(candidate, removeEc);
    }
}

std::filesystem::path ResolveAssetPathByGuid(AssetManager* assetManager, const GUID& guid,
                                             const std::filesystem::path& assetsRoot)
{
    if (assetManager == nullptr || guid.IsNull())
        return {};
    AssetMetadata meta{};
    if (!assetManager->GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return {};
    std::filesystem::path path = meta.Path;
    if (path.is_relative() && !assetsRoot.empty())
        path = assetsRoot.parent_path() / path;
    return path.lexically_normal();
}

} // namespace Editor
} // namespace GameEngine
