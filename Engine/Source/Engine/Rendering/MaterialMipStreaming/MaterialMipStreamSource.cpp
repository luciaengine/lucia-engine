#include "Engine/Rendering/MaterialMipStreaming/MaterialMipStreamSource.h"

#include "Assets/AssetIOService.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

namespace
{
// A level with a longer edge than this is streamed. The smaller levels are the tail.
constexpr uint32 kTailExtent = 256;
} // namespace

std::optional<MaterialMipStreamSource::Chain> MaterialMipStreamSource::ChainOf(const TextureAsset& texture,
                                                                               Rendering::TextureFormat format)
{
    const TextureLevelFile* file = texture.GetLevelFile();
    if (!file)
        return std::nullopt;
    Chain chain{*file, {}, format};
    chain.Extents.reserve(texture.GetMipChain().size());
    for (const TextureMipDesc& level : texture.GetMipChain())
        chain.Extents.push_back({level.Width, level.Height});
    return chain;
}

std::shared_ptr<const MaterialMipStreamSource> MaterialMipStreamSource::Create(
    const TextureAsset& texture, Rendering::TextureFormat format, Rendering::TextureFormat companionFormat)
{
    std::optional<Chain> chain = ChainOf(texture, format);
    if (!chain)
        return nullptr;
    const auto tail = std::find_if(chain->Extents.begin(), chain->Extents.end(), [](const LevelExtent& level)
                                   { return std::max(level.Width, level.Height) <= kTailExtent; });
    if (tail == chain->Extents.begin() || tail == chain->Extents.end())
        return nullptr;

    auto source = std::make_shared<MaterialMipStreamSource>();
    source->m_Tail = static_cast<uint32>(tail - chain->Extents.begin());
    source->m_TopExtent = std::max(chain->Extents.front().Width, chain->Extents.front().Height);
    source->m_Texture = std::move(*chain);
    if (const TextureAsset* companion = texture.GetNormalVarianceCompanion())
    {
        source->m_Companion = ChainOf(*companion, companionFormat);
        if (!source->m_Companion)
            return nullptr;
    }

    // A file stores its smallest level first: the bytes from a level to the end of the image data.
    const auto levels = source->m_Texture.File.Levels();
    source->m_SuffixBytes.resize(levels.size());
    uint64 bytes = 0;
    for (size_t level = levels.size(); level-- > 0;)
    {
        bytes += levels[level].Bytes;
        uint64 companionBytes = 0;
        if (source->m_Companion)
        {
            const auto companionLevels = source->m_Companion->File.Levels();
            for (size_t mip = source->CompanionMip(static_cast<uint32>(level)); mip < companionLevels.size(); ++mip)
                companionBytes += companionLevels[mip].Bytes;
        }
        source->m_SuffixBytes[level] = bytes + companionBytes;
    }
    return source;
}

uint32 MaterialMipStreamSource::CompanionMip(uint32 firstMip) const
{
    return std::min(firstMip, static_cast<uint32>(m_Companion->File.Levels().size() - 1));
}

void MaterialMipStreamSource::StoreFile(LevelRead& read, ByteBuffer LevelRead::* target, const TextureLevelFile& file,
                                        uint64 prefixBytes, ByteBuffer bytes)
{
    if (bytes.Size() == prefixBytes && file.Matches({bytes.Data(), bytes.Size()}))
        read.*target = std::move(bytes);
    else
        read.Failed.store(true, std::memory_order_release);
    read.PendingFiles.fetch_sub(1, std::memory_order_release);
}

void MaterialMipStreamSource::FailFile(LevelRead& read)
{
    read.Failed.store(true, std::memory_order_release);
    read.PendingFiles.fetch_sub(1, std::memory_order_release);
}

void MaterialMipStreamSource::SubmitFileRead(AssetManager& assets, const GUID& texture, const TextureLevelFile& file,
                                             uint32 firstLevel, const std::shared_ptr<LevelRead>& read,
                                             ByteBuffer LevelRead::* target)
{
    const uint64 prefixBytes = file.PrefixBytes(firstLevel);
    AssetIOService::ReadRequest request;
    request.AssetGuid = texture;
    // Not AssetType::Texture: the read runs no cook, so it does not wait for a texture decode slot.
    request.Metadata = AssetMetadata(texture, AssetType::Unknown, file.Path());
    request.Priority = AssetLoadPriority::High;
    request.RangeBytes = prefixBytes;
    request.MaximumBytes = prefixBytes;
    request.CancelRequested = read->Cancel;
    request.ProcessData = [read, target, file, prefixBytes](ByteBuffer bytes) -> SharedPtr<Asset>
    {
        StoreFile(*read, target, file, prefixBytes, std::move(bytes));
        return nullptr;
    };
    request.OnFailure = [read](const String&) { FailFile(*read); };
    assets.SubmitRead(std::move(request));
}

std::shared_ptr<MaterialMipStreamSource::LevelRead> MaterialMipStreamSource::ReadLevels(
    const std::shared_ptr<const MaterialMipStreamSource>& source, AssetManager& assets, const GUID& texture,
    uint32 firstMip)
{
    auto read = std::make_shared<LevelRead>();
    read->FirstMip = firstMip;
    read->PendingFiles.store(source->m_Companion ? 2 : 1, std::memory_order_release);
    SubmitFileRead(assets, texture, source->m_Texture.File, firstMip, read, &LevelRead::Texture);
    if (source->m_Companion)
        SubmitFileRead(assets, texture, source->m_Companion->File, source->CompanionMip(firstMip), read,
                       &LevelRead::Companion);
    return read;
}

std::vector<Rendering::TextureMipUploadEntry> MaterialMipStreamSource::LevelsOf(const Chain& chain, uint32 firstLevel,
                                                                               const ByteBuffer& prefix)
{
    const auto levels = chain.File.Levels();
    std::vector<Rendering::TextureMipUploadEntry> entries;
    entries.reserve(levels.size() - firstLevel);
    for (size_t level = firstLevel; level < levels.size(); ++level)
        entries.push_back({prefix.Data() + levels[level].Offset, static_cast<size_t>(levels[level].Bytes),
                           chain.Extents[level].Width, chain.Extents[level].Height});
    return entries;
}

std::vector<Rendering::TextureMipUploadEntry> MaterialMipStreamSource::TextureLevels(const LevelRead& read) const
{
    return LevelsOf(m_Texture, read.FirstMip, read.Texture);
}

std::vector<Rendering::TextureMipUploadEntry> MaterialMipStreamSource::CompanionLevels(const LevelRead& read) const
{
    return LevelsOf(*m_Companion, CompanionMip(read.FirstMip), read.Companion);
}

} // namespace GameEngine::Engine::Renderer
