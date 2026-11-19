#pragma once

#include "AssetCore/GUID.h"
#include "Assets/TextureLevelFile.h"
#include "Types/ByteBuffer.h"
#include "Types/Types.h"

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine
{
class AssetManager;
class TextureAsset;
} // namespace GameEngine

namespace GameEngine::Rendering
{
enum class TextureFormat : uint32_t;
struct TextureMipUploadEntry;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

/// Where a material mip stream gets its levels: the cooked file of the texture and, for a normal
/// map, the file of its variance companion. It holds no texels. A step reads the levels that it
/// makes resident from the files.
class MaterialMipStreamSource
{
  public:
    /// One read of the levels FirstMip..last. The I/O service fills it. Read Texture and Companion
    /// only when Done() is true and Failed is false.
    struct LevelRead
    {
        uint32 FirstMip = 0;
        ByteBuffer Texture;   ///< The texture file from its start through the level FirstMip.
        ByteBuffer Companion; ///< The companion file, the same. Empty without a companion.
        std::atomic<uint32> PendingFiles{0};
        std::atomic<bool> Failed{false}; ///< A file did not read, or is not the container of the stream.
        std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);

        bool Done() const { return PendingFiles.load(std::memory_order_acquire) == 0; }
    };

    /// The source of `texture`, which uploads as `format` and its companion as `companionFormat`.
    /// Null when a file of the texture gives no level range (TextureAsset::GetLevelFile), or when
    /// the texture has no level above its tail.
    static std::shared_ptr<const MaterialMipStreamSource> Create(const TextureAsset& texture,
                                                                 Rendering::TextureFormat format,
                                                                 Rendering::TextureFormat companionFormat);

    /// Starts the read of the levels `firstMip`..last of `source`, the stream of `texture`.
    static std::shared_ptr<LevelRead> ReadLevels(const std::shared_ptr<const MaterialMipStreamSource>& source,
                                                 AssetManager& assets, const GUID& texture, uint32 firstMip);

    /// The first level of the mip tail: the stream always keeps this level and the smaller ones.
    uint32 Tail() const { return m_Tail; }
    /// The longest edge of level 0, in texels.
    uint32 TopExtent() const { return m_TopExtent; }
    /// [i] is the bytes of the texture and its companion when level i is the first resident level.
    std::span<const uint64> SuffixBytes() const { return m_SuffixBytes; }
    Rendering::TextureFormat Format() const { return m_Texture.Format; }
    Rendering::TextureFormat CompanionFormat() const { return m_Companion->Format; }
    bool HasCompanion() const { return m_Companion.has_value(); }
    /// The first companion level when `firstMip` is the first level of the texture. The companion
    /// can have fewer levels. Call only when HasCompanion().
    uint32 CompanionMip(uint32 firstMip) const;

    /// The levels of a finished read, largest first. The entries point into `read`.
    std::vector<Rendering::TextureMipUploadEntry> TextureLevels(const LevelRead& read) const;
    std::vector<Rendering::TextureMipUploadEntry> CompanionLevels(const LevelRead& read) const;

  private:
    struct LevelExtent
    {
        uint32 Width = 0;
        uint32 Height = 0;
    };
    // One file of the stream.
    struct Chain
    {
        TextureLevelFile File;
        std::vector<LevelExtent> Extents; // base level first, as File.Levels()
        Rendering::TextureFormat Format{};
    };

    static std::optional<Chain> ChainOf(const TextureAsset& texture, Rendering::TextureFormat format);
    static std::vector<Rendering::TextureMipUploadEntry> LevelsOf(const Chain& chain, uint32 firstLevel,
                                                                 const ByteBuffer& prefix);
    static void SubmitFileRead(AssetManager& assets, const GUID& texture, const TextureLevelFile& file,
                               uint32 firstLevel, const std::shared_ptr<LevelRead>& read,
                               ByteBuffer LevelRead::* target);
    // The two ends of a file read. Each runs one time for each file, on an I/O or job thread.
    static void StoreFile(LevelRead& read, ByteBuffer LevelRead::* target, const TextureLevelFile& file,
                          uint64 prefixBytes, ByteBuffer bytes);
    static void FailFile(LevelRead& read);

    Chain m_Texture;
    std::optional<Chain> m_Companion;
    std::vector<uint64> m_SuffixBytes;
    uint32 m_Tail = 0;
    uint32 m_TopExtent = 1;
};

} // namespace GameEngine::Engine::Renderer
