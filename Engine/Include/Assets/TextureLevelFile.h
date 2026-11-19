#pragma once

#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine
{

/// Where the mip levels of a KTX2 container are in its file.
///
/// A KTX2 file stores its smallest level first. The bytes [0, PrefixBytes(first)) thus hold the
/// header, the level index and the levels first..last: one read gives a mip tail without the
/// larger levels.
class TextureLevelFile
{
  public:
    struct Level
    {
        uint64 Offset = 0; ///< From the start of the file.
        uint64 Bytes = 0;

        bool operator==(const Level&) const = default;
    };

    /// The level index of the KTX2 file `file`, which `path` names. Nullopt when the bytes are not
    /// KTX2, when a Basis payload or a supercompression scheme makes the stored bytes differ from
    /// the texels, or when a level is outside the file or is not before the next larger level.
    static std::optional<TextureLevelFile> Parse(std::span<const uint8> file, std::filesystem::path path);

    const std::filesystem::path& Path() const { return m_Path; }
    /// Base level first.
    std::span<const Level> Levels() const { return m_Levels; }
    uint64 PrefixBytes(uint32 firstLevel) const { return m_Levels[firstLevel].Offset + m_Levels[firstLevel].Bytes; }

    /// True when `prefix`, the first bytes of the file now, has this header and this level index:
    /// the same format, dimensions and level layout. False after a container with another of
    /// these replaced the file. Texels that changed inside the same layout are not seen.
    bool Matches(std::span<const uint8> prefix) const;

  private:
    // The KTX2 header: the format, the dimensions, the level count and the metadata ranges.
    static constexpr size_t kHeaderBytes = 80;

    std::filesystem::path m_Path;
    std::array<uint8, kHeaderBytes> m_Header{};
    std::vector<Level> m_Levels;
};

} // namespace GameEngine
