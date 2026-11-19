// The ocean's CPU copies of its author maps: one conversion for each payload of a texture, and
// no pixels taken from the asset when the copy is in the cache. Headless: no device, no engine.

#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Ocean/OceanCpuTextureCache.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace GameEngine;
using GameEngine::Ocean::OceanCpuTextureCache;
using GameEngine::Ocean::OceanCpuTextureRG;

namespace
{
namespace fs = std::filesystem;

constexpr uint32 kSize = 64;

// A cooked RGBA8 container of kSize texels a side. Red is `red` in every texel.
std::vector<uint8> CookedContainer(uint8 red, TextureColorSpace colorSpace = TextureColorSpace::Linear)
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
    TextureCookInputs inputs;
    inputs.ColorSpace = colorSpace;
    EXPECT_TRUE(CookTexture(tga.data(), tga.size(), "map.tga", ".tga", inputs, TextureCookOutput::Uncompressed,
                            cooked, error))
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
        ASSERT_EQ(texture.AcquirePixels().Pixels, nullptr);
    }

    fs::path m_Directory;
    fs::path m_File;
    GUID m_Guid = GUID::Generate();
    OceanCpuTextureCache m_Cache;
};

TEST_F(OceanCpuTextureCacheTest, TheCopyHoldsTheRedAndGreenOfTheTexels)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const auto copy = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(copy, nullptr);
    ASSERT_TRUE(copy->IsValid());
    EXPECT_EQ(copy->Width, kSize);
    EXPECT_EQ(copy->Height, kSize);
    // A linear container: red 50 and green 80 of 255, as the sampler gives them.
    EXPECT_NEAR(copy->RG[0], 50.0f / 255.0f, 0.0001f);
    EXPECT_NEAR(copy->RG[1], 80.0f / 255.0f, 0.0001f);
    EXPECT_EQ(copy->RG[(kSize * kSize - 1) * 2], copy->RG[0]);
}

TEST_F(OceanCpuTextureCacheTest, TheCopyOfAnSrgbTextureIsLinear)
{
    ASSERT_NO_FATAL_FAILURE(WriteMap(CookedContainer(50, TextureColorSpace::SRGB)));
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    ASSERT_EQ(texture.GetColorSpace(), TextureColorSpace::SRGB);
    const auto copy = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(copy, nullptr);
    // The sRGB values 50 and 80 of 255, decoded as the sampler of an sRGB texture decodes them.
    EXPECT_NEAR(copy->RG[0], 0.0319f, 0.0005f);
    EXPECT_NEAR(copy->RG[1], 0.0802f, 0.0005f);
}

TEST_F(OceanCpuTextureCacheTest, ASecondReadOfATextureSharesTheCopyAndTakesNoPixelsFromTheAsset)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const auto first = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(first, nullptr);

    ASSERT_NO_FATAL_FAILURE(TakeThePixelsAway(texture));
    EXPECT_EQ(m_Cache.Get(m_Guid, texture), first);
    // With no cache, the same texture gives no copy now.
    EXPECT_FALSE(OceanCpuTextureCache::Convert(texture.AcquirePixels()).IsValid());
}

TEST_F(OceanCpuTextureCacheTest, AReloadOfTheTextureGivesANewCopy)
{
    ReloadableTexture texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const auto first = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(first, nullptr);

    ASSERT_TRUE(texture.ReloadFromData(CookedContainer(200)));
    const auto second = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(second, nullptr);
    EXPECT_GT(second->RG[0], first->RG[0]);
    EXPECT_EQ(second->RG[1], first->RG[1]);
}

TEST_F(OceanCpuTextureCacheTest, AnUnloadAndALoadOfTheTextureGiveANewCopy)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const auto first = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(first, nullptr);

    texture.Unload();
    ASSERT_NO_FATAL_FAILURE(WriteMap(CookedContainer(200)));
    ASSERT_TRUE(texture.Load());
    const auto second = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(second, nullptr);
    EXPECT_GT(second->RG[0], first->RG[0]);
}

TEST_F(OceanCpuTextureCacheTest, AnotherAssetWithTheSameGuidGivesANewCopy)
{
    std::shared_ptr<const OceanCpuTextureRG> first;
    uint64 generation = 0;
    {
        TextureAsset texture(m_Guid, m_File);
        ASSERT_TRUE(texture.Load());
        first = m_Cache.Get(m_Guid, texture);
        ASSERT_NE(first, nullptr);
        generation = texture.GetPayloadGeneration();
    }
    ASSERT_NO_FATAL_FAILURE(WriteMap(CookedContainer(200)));
    // A new asset never has the generation of another one, also at the same address.
    TextureAsset other(m_Guid, m_File);
    ASSERT_TRUE(other.Load());
    EXPECT_NE(other.GetPayloadGeneration(), generation);
    const auto second = m_Cache.Get(m_Guid, other);
    ASSERT_NE(second, nullptr);
    EXPECT_GT(second->RG[0], first->RG[0]);
}

TEST_F(OceanCpuTextureCacheTest, ACopyThatEveryUpdateReadsStays)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    const auto first = m_Cache.Get(m_Guid, texture);
    ASSERT_NE(first, nullptr);
    ASSERT_NO_FATAL_FAILURE(TakeThePixelsAway(texture));
    constexpr int kUpdates = 4;
    for (int update = 0; update < kUpdates; ++update)
    {
        m_Cache.DropUnused();
        EXPECT_EQ(m_Cache.Get(m_Guid, texture), first) << "update " << update;
    }
}

TEST_F(OceanCpuTextureCacheTest, ACopyThatNoUpdateReadsIsDropped)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    ASSERT_NE(m_Cache.Get(m_Guid, texture), nullptr);
    ASSERT_NO_FATAL_FAILURE(TakeThePixelsAway(texture));

    // 2 updates with no read drop it: the next read converts again and finds no pixels.
    m_Cache.DropUnused();
    m_Cache.DropUnused();
    EXPECT_EQ(m_Cache.Get(m_Guid, texture), nullptr);
}

TEST_F(OceanCpuTextureCacheTest, AConversionThatFailsIsTriedAgain)
{
    TextureAsset texture(m_Guid, m_File);
    ASSERT_TRUE(texture.Load());
    ASSERT_NE(texture.GetLevelFile(), nullptr);
    const fs::path away = m_Directory / "away.ktx2";
    texture.ReleaseLevelPixels();
    fs::rename(m_File, away);
    EXPECT_EQ(m_Cache.Get(m_Guid, texture), nullptr);

    // The file is back and the payload is the same one: the next read gives the copy.
    fs::rename(away, m_File);
    EXPECT_NE(m_Cache.Get(m_Guid, texture), nullptr);
}

} // namespace
