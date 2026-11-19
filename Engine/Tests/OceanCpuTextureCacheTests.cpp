// The ocean's CPU copies of its author maps: one conversion for each payload of a texture, and
// no pixels taken from the asset when the copy is in the cache. Headless: no device, no engine.

#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Ocean/OceanCpuTextureCache.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;
using GameEngine::Ocean::OceanCpuTextureCache;
using GameEngine::Ocean::OceanCpuTextureRG;

namespace
{
namespace fs = std::filesystem;

constexpr uint32 kSize = 64;

// A cooked RGBA8 container of kSize texels a side. Red is `red` in every texel.
std::vector<uint8> CookedContainer(uint8 red)
{
    std::vector<uint8> tga(18, 0);
    tga[2] = 2; // uncompressed true color
    tga[12] = kSize;
    tga[14] = kSize;
    tga[16] = 32;
    tga[17] = 0x28;
    for (uint32 texel = 0; texel < kSize * kSize; ++texel)
        tga.insert(tga.end(), {40, 80, red, 255}); // blue, green, red, alpha
    std::vector<uint8> cooked;
    std::string error;
    EXPECT_TRUE(CookTexture(tga.data(), tga.size(), "map.tga", ".tga", TextureCookInputs{},
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;
    return cooked;
}

// The reload entry that Asset::Reload calls with the bytes of the file.
class ReloadableTexture : public TextureAsset
{
  public:
    using TextureAsset::ReloadFromData;
    using TextureAsset::TextureAsset;
};

class OceanCpuTextureCacheTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Directory = TestUtils::MakeUniqueTempDirectory("ge-ocean-cpu-texture");
        fs::create_directories(m_Directory);
        m_File = m_Directory / "map.ktx2";
        WriteMap(CookedContainer(50));
    }
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(m_Directory, error);
    }
    void WriteMap(const std::vector<uint8>& bytes)
    {
        std::ofstream stream(m_File, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream.good());
    }
    // The asset with no pixels and no file to read them from: only a cached copy can answer.
    void TakeThePixelsAway(TextureAsset& texture)
    {
        ASSERT_NE(texture.GetLevelFile(), nullptr);
        texture.ReleaseLevelPixels();
        fs::remove(m_File);
        ASSERT_EQ(texture.AcquirePixels(), nullptr);
    }

    fs::path m_Directory;
    fs::path m_File;
    GUID m_Guid = GUID::Generate();
    OceanCpuTextureCache m_Cache;
};

TEST_F(OceanCpuTextureCacheTest, ASecondReadOfATextureTakesNoPixelsFromTheAsset)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const OceanCpuTextureRG first = m_Cache.Get(m_Guid, texture);
    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(first.Width, kSize);
    EXPECT_EQ(first.Height, kSize);

    ASSERT_NO_FATAL_FAILURE(TakeThePixelsAway(texture));
    const OceanCpuTextureRG& second = m_Cache.Get(m_Guid, texture);
    ASSERT_TRUE(second.IsValid());
    EXPECT_EQ(second.RG, first.RG);
    // With no cache, the same texture gives no copy now.
    EXPECT_FALSE(OceanCpuTextureCache::Convert(texture).IsValid());
}

TEST_F(OceanCpuTextureCacheTest, AReloadOfTheTextureGivesANewCopy)
{
    ReloadableTexture texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const OceanCpuTextureRG first = m_Cache.Get(m_Guid, texture);
    ASSERT_TRUE(first.IsValid());

    ASSERT_TRUE(texture.ReloadFromData(CookedContainer(200)));
    const OceanCpuTextureRG& second = m_Cache.Get(m_Guid, texture);
    ASSERT_TRUE(second.IsValid());
    EXPECT_GT(second.RG[0], first.RG[0]);
    EXPECT_EQ(second.RG[1], first.RG[1]);
}

TEST_F(OceanCpuTextureCacheTest, AnotherAssetWithTheSameGuidGivesANewCopy)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const OceanCpuTextureRG first = m_Cache.Get(m_Guid, texture);
    ASSERT_TRUE(first.IsValid());

    ASSERT_NO_FATAL_FAILURE(WriteMap(CookedContainer(200)));
    TextureAsset other(m_Guid, m_File);
    ASSERT_TRUE(other.Load());
    EXPECT_GT(m_Cache.Get(m_Guid, other).RG[0], first.RG[0]);
}

TEST_F(OceanCpuTextureCacheTest, ACopyThatNoUpdateReadsIsDropped)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    ASSERT_TRUE(m_Cache.Get(m_Guid, texture).IsValid());
    ASSERT_NO_FATAL_FAILURE(TakeThePixelsAway(texture));

    // The update after the read keeps the copy.
    m_Cache.DropUnused();
    EXPECT_TRUE(m_Cache.Get(m_Guid, texture).IsValid());
    // 2 updates with no read drop it: the next read converts again and finds no pixels.
    m_Cache.DropUnused();
    m_Cache.DropUnused();
    EXPECT_FALSE(m_Cache.Get(m_Guid, texture).IsValid());
}

} // namespace
