#include "Assets/TextureLevelFile.h"

#include <algorithm>
#include <array>

namespace GameEngine
{

namespace
{
// The KTX2 header layout (KTX 2.0 specification, section 3). All values are little-endian.
constexpr std::array<uint8, 12> kKtx2Identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr size_t kVkFormatOffset = 12;
constexpr size_t kLevelCountOffset = 40;
constexpr size_t kSupercompressionOffset = 44;
constexpr size_t kLevelIndexOffset = 80;
// Per level, base level first: byteOffset, byteLength, uncompressedByteLength.
constexpr size_t kLevelIndexEntryBytes = 24;
// A 2D texture has at most 32 levels. A larger count is not a texture.
constexpr uint32 kMaximumLevelCount = 32;

uint64 ReadLittleEndian(std::span<const uint8> bytes, size_t offset, size_t width)
{
    uint64 value = 0;
    for (size_t byte = 0; byte < width; ++byte)
        value |= static_cast<uint64>(bytes[offset + byte]) << (8 * byte);
    return value;
}

// The level index of the container that `bytes` starts. False when `bytes` does not hold a whole
// index of a container that stores its texels as they are.
bool ReadLevelIndex(std::span<const uint8> bytes, std::vector<TextureLevelFile::Level>& levels)
{
    if (bytes.size() < kLevelIndexOffset ||
        !std::equal(kKtx2Identifier.begin(), kKtx2Identifier.end(), bytes.begin()))
        return false;
    const uint64 vkFormat = ReadLittleEndian(bytes, kVkFormatOffset, sizeof(uint32));
    const uint64 supercompression = ReadLittleEndian(bytes, kSupercompressionOffset, sizeof(uint32));
    // A container with one image and no mip chain has the level count 0.
    const uint64 levelCount = std::max<uint64>(ReadLittleEndian(bytes, kLevelCountOffset, sizeof(uint32)), 1);
    if (vkFormat == 0 || supercompression != 0 || levelCount > kMaximumLevelCount ||
        bytes.size() - kLevelIndexOffset < levelCount * kLevelIndexEntryBytes)
        return false;

    levels.resize(levelCount);
    for (size_t level = 0; level < levelCount; ++level)
    {
        const size_t entry = kLevelIndexOffset + level * kLevelIndexEntryBytes;
        levels[level].Offset = ReadLittleEndian(bytes, entry, sizeof(uint64));
        levels[level].Bytes = ReadLittleEndian(bytes, entry + sizeof(uint64), sizeof(uint64));
    }
    return true;
}
} // namespace

std::optional<TextureLevelFile> TextureLevelFile::Parse(std::span<const uint8> file, std::filesystem::path path)
{
    TextureLevelFile result;
    if (!ReadLevelIndex(file, result.m_Levels))
        return std::nullopt;
    uint64 end = file.size();
    for (const Level& level : result.m_Levels)
    {
        if (level.Bytes == 0 || level.Offset > end || level.Bytes > end - level.Offset)
            return std::nullopt;
        end = level.Offset;
    }
    result.m_Path = std::move(path);
    return result;
}

bool TextureLevelFile::Matches(std::span<const uint8> prefix) const
{
    std::vector<Level> levels;
    return ReadLevelIndex(prefix, levels) && std::ranges::equal(levels, m_Levels);
}

} // namespace GameEngine
