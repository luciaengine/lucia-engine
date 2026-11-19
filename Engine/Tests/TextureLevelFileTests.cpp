// The level index of a cooked KTX2 file, and a texture asset that releases its pixels and reads
// them again from that file. Headless: no GPU device and no engine.

#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Assets/TextureLevelFile.h"
#include "TestTempDir.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;

namespace
{
namespace fs = std::filesystem;

// The KTX2 header offsets that the tests change (KTX 2.0 specification, section 3).
constexpr size_t kVkFormatOffset = 12;
constexpr size_t kSupercompressionOffset = 44;
constexpr size_t kLevelIndexOffset = 80;

// A cooked RGBA8 container of `size` texels a side with its full mip chain.
std::vector<uint8> CookedContainer(uint32 size)
{
    std::vector<uint8> tga(18, 0);
    tga[2] = 2; // uncompressed true color
    tga[12] = static_cast<uint8>(size & 0xff);
    tga[13] = static_cast<uint8>(size >> 8);
    tga[14] = static_cast<uint8>(size & 0xff);
    tga[15] = static_cast<uint8>(size >> 8);
    tga[16] = 32;
    tga[17] = 0x28;
    for (uint32 y = 0; y < size; ++y)
        for (uint32 x = 0; x < size; ++x)
            tga.insert(tga.end(), {static_cast<uint8>(x * 3), static_cast<uint8>(y * 5), static_cast<uint8>(x + y), 255});
    std::vector<uint8> cooked;
    std::string error;
    EXPECT_TRUE(CookTexture(tga.data(), tga.size(), "levels.tga", ".tga", TextureCookInputs{},
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;
    return cooked;
}

void WriteFile(const fs::path& path, const std::vector<uint8>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

class TextureLevelFileTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Directory = TestUtils::MakeUniqueTempDirectory("ge-texture-level-file");
        fs::create_directories(m_Directory);
    }
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(m_Directory, error);
    }

    fs::path m_Directory;
};

TEST_F(TextureLevelFileTest, AFilePrefixHoldsTheLevelsThatTheAssetLoads)
{
    const std::vector<uint8> cooked = CookedContainer(64);
    const fs::path path = m_Directory / "levels.ktx2";
    ASSERT_NO_FATAL_FAILURE(WriteFile(path, cooked));
    TextureAsset asset(GUID::Generate(), path);
    ASSERT_TRUE(asset.Load());
    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 7u);

    const auto file = TextureLevelFile::Parse(cooked, path);
    ASSERT_TRUE(file.has_value());
    ASSERT_EQ(file->Levels().size(), chain.size());
    EXPECT_EQ(file->PrefixBytes(0), cooked.size());
    for (uint32 first = 0; first < chain.size(); ++first)
    {
        // The read of a step: the file from its start through the level `first`.
        const std::span<const uint8> prefix(cooked.data(), static_cast<size_t>(file->PrefixBytes(first)));
        EXPECT_TRUE(file->Matches(prefix));
        for (size_t level = first; level < chain.size(); ++level)
        {
            ASSERT_LE(file->Levels()[level].Offset + file->Levels()[level].Bytes, prefix.size());
            ASSERT_EQ(file->Levels()[level].Bytes, chain[level].Size);
            EXPECT_EQ(std::memcmp(prefix.data() + file->Levels()[level].Offset,
                                  asset.GetPixelData() + chain[level].Offset, chain[level].Size),
                      0)
                << "level " << level << " from the prefix of level " << first;
        }
    }
    // The smallest level is first in the file: a smaller tail is a shorter read.
    EXPECT_LT(file->PrefixBytes(3), file->PrefixBytes(2));
}

TEST_F(TextureLevelFileTest, AContainerThatDoesNotStoreItsTexelsGivesNoLevels)
{
    const std::vector<uint8> cooked = CookedContainer(16);
    ASSERT_TRUE(TextureLevelFile::Parse(cooked, "a.ktx2").has_value());

    std::vector<uint8> supercompressed = cooked;
    supercompressed[kSupercompressionOffset] = 2; // Zstandard
    EXPECT_FALSE(TextureLevelFile::Parse(supercompressed, "a.ktx2").has_value());

    std::vector<uint8> basis = cooked;
    std::memset(basis.data() + kVkFormatOffset, 0, sizeof(uint32)); // VK_FORMAT_UNDEFINED
    EXPECT_FALSE(TextureLevelFile::Parse(basis, "a.ktx2").has_value());

    std::vector<uint8> notKtx = cooked;
    notKtx[0] = 0;
    EXPECT_FALSE(TextureLevelFile::Parse(notKtx, "a.ktx2").has_value());

    // The file ends before the end of its base level.
    const std::span<const uint8> cut(cooked.data(), cooked.size() - 1);
    EXPECT_FALSE(TextureLevelFile::Parse(cut, "a.ktx2").has_value());
    EXPECT_FALSE(TextureLevelFile::Parse(std::span(cooked).first(kLevelIndexOffset), "a.ktx2").has_value());
}

TEST_F(TextureLevelFileTest, AnotherContainerDoesNotMatch)
{
    const std::vector<uint8> cooked = CookedContainer(64);
    const std::vector<uint8> other = CookedContainer(32);
    const auto file = TextureLevelFile::Parse(cooked, "a.ktx2");
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(file->Matches(cooked));
    EXPECT_FALSE(file->Matches(other));
    EXPECT_FALSE(file->Matches(std::span(cooked).first(kLevelIndexOffset - 1)));
    EXPECT_FALSE(file->Matches({}));
}

TEST_F(TextureLevelFileTest, AnAssetReleasesItsPixelsAndReadsThemAgain)
{
    const std::vector<uint8> cooked = CookedContainer(64);
    const fs::path path = m_Directory / "release.ktx2";
    ASSERT_NO_FATAL_FAILURE(WriteFile(path, cooked));
    TextureAsset asset(GUID::Generate(), path);
    ASSERT_TRUE(asset.Load());
    ASSERT_NE(asset.GetLevelFile(), nullptr);
    EXPECT_EQ(asset.GetLevelFile()->Path(), path);
    const std::vector<uint8> pixels(asset.GetPixelData(), asset.GetPixelData() + asset.GetDataSize());

    asset.ReleaseLevelPixels();
    EXPECT_EQ(asset.GetPixelData(), nullptr);
    EXPECT_EQ(asset.GetMipChain().size(), 7u);
    EXPECT_EQ(asset.GetWidth(), 64u);

    ASSERT_TRUE(asset.RestoreLevelPixels());
    ASSERT_NE(asset.GetPixelData(), nullptr);
    EXPECT_EQ(std::memcmp(asset.GetPixelData(), pixels.data(), pixels.size()), 0);

    // Another container replaced the file: the pixels do not come back as the wrong texture.
    asset.ReleaseLevelPixels();
    ASSERT_NO_FATAL_FAILURE(WriteFile(path, CookedContainer(32)));
    EXPECT_FALSE(asset.RestoreLevelPixels());
    EXPECT_EQ(asset.GetPixelData(), nullptr);
}

TEST_F(TextureLevelFileTest, BytesThatNoFileHoldsGiveNoLevelFile)
{
    const std::vector<uint8> cooked = CookedContainer(64);
    // No file at the path of the asset.
    TextureAsset absent(GUID::Generate(), m_Directory / "absent.ktx2");
    ASSERT_TRUE(absent.LoadFromData(cooked));
    EXPECT_EQ(absent.GetLevelFile(), nullptr);
    absent.ReleaseLevelPixels();
    EXPECT_NE(absent.GetPixelData(), nullptr);

    // The file at the path holds other bytes than the asset loaded.
    const fs::path path = m_Directory / "other.ktx2";
    ASSERT_NO_FATAL_FAILURE(WriteFile(path, CookedContainer(32)));
    TextureAsset other(GUID::Generate(), path);
    ASSERT_TRUE(other.LoadFromData(cooked));
    EXPECT_EQ(other.GetLevelFile(), nullptr);
}

} // namespace
