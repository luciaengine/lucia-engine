// A texture asset that reloads in place: the decode goes into a staged payload, so a failed
// reload keeps the pixels and a reader on another thread never sees the asset without them.
// Headless: no GPU device and no engine.

#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

// A cooked RGBA8 container of 64 texels a side. `seed` changes every texel.
std::vector<uint8> CookedContainer(uint8 seed)
{
    constexpr uint32 kSize = 64;
    std::vector<uint8> tga(18, 0);
    tga[2] = 2; // uncompressed true color
    tga[12] = kSize;
    tga[14] = kSize;
    tga[16] = 32;
    tga[17] = 0x28;
    for (uint32 y = 0; y < kSize; ++y)
        for (uint32 x = 0; x < kSize; ++x)
            tga.insert(tga.end(), {static_cast<uint8>(x + seed), static_cast<uint8>(y + seed), seed, 255});
    std::vector<uint8> cooked;
    std::string error;
    EXPECT_TRUE(CookTexture(tga.data(), tga.size(), "reload.tga", ".tga", TextureCookInputs{},
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

std::vector<uint8> PixelsOf(const TextureAsset& asset)
{
    return std::vector<uint8>(asset.GetPixelData(), asset.GetPixelData() + asset.GetDataSize());
}

TEST(TextureAssetReload, AFailedReloadInPlaceKeepsThePixels)
{
    ReloadableTexture asset(GUID::Generate(), "reload.ktx2");
    ASSERT_TRUE(asset.LoadFromData(CookedContainer(10)));
    const std::vector<uint8> pixels = PixelsOf(asset);

    const std::vector<uint8> notATexture{'n', 'o', 't', ' ', 'k', 't', 'x'};
    EXPECT_FALSE(asset.ReloadFromData(notATexture));
    EXPECT_TRUE(asset.IsLoaded());
    ASSERT_NE(asset.GetPixelData(), nullptr);
    EXPECT_EQ(asset.GetDataSize(), pixels.size());
    EXPECT_EQ(std::memcmp(asset.GetPixelData(), pixels.data(), pixels.size()), 0);
}

TEST(TextureAssetReload, AReloadInPlaceOfATextureThatIsNotLoadedSetsItsState)
{
    ReloadableTexture asset(GUID::Generate(), "reload.ktx2");
    const std::vector<uint8> notATexture{'n', 'o', 't', ' ', 'k', 't', 'x'};
    EXPECT_FALSE(asset.ReloadFromData(notATexture));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);

    ASSERT_TRUE(asset.ReloadFromData(CookedContainer(10)));
    EXPECT_TRUE(asset.IsLoaded());
    EXPECT_NE(asset.GetPixelData(), nullptr);
}

TEST(TextureAssetReload, AReloadInPlaceReplacesThePixels)
{
    ReloadableTexture asset(GUID::Generate(), "reload.ktx2");
    ASSERT_TRUE(asset.LoadFromData(CookedContainer(10)));
    const std::vector<uint8> first = PixelsOf(asset);
    const std::shared_ptr<const uint8[]> held = asset.AcquirePixels();

    ASSERT_TRUE(asset.ReloadFromData(CookedContainer(200)));
    EXPECT_TRUE(asset.IsLoaded());
    ASSERT_EQ(asset.GetDataSize(), first.size());
    EXPECT_NE(std::memcmp(asset.GetPixelData(), first.data(), first.size()), 0);
    // A reader keeps the pixels that it acquired before the reload.
    EXPECT_EQ(std::memcmp(held.get(), first.data(), first.size()), 0);
}

TEST(TextureAssetReload, AReaderOnAnotherThreadAlwaysGetsAWholePayloadDuringReloadsInPlace)
{
    const std::vector<uint8> containers[2] = {CookedContainer(10), CookedContainer(200)};
    ReloadableTexture asset(GUID::Generate(), "reload.ktx2");
    ASSERT_TRUE(asset.LoadFromData(containers[0]));
    const std::vector<uint8> firstPixels = PixelsOf(asset);
    ASSERT_TRUE(asset.ReloadFromData(containers[1]));
    const std::vector<uint8> secondPixels = PixelsOf(asset);
    ASSERT_EQ(firstPixels.size(), secondPixels.size());

    // The owner makes kReloads reloads and then more until the reader has made kReads reads, so
    // reads and reloads overlap. The deadline ends a run where the reader does not start.
    constexpr int kReloads = 200;
    constexpr int kReads = 200;
    std::atomic<bool> done{false};
    std::atomic<int> reads{0};
    std::atomic<int> wrongReads{0};
    std::thread reader([&asset, &firstPixels, &secondPixels, &done, &reads, &wrongReads]
    {
        while (!done.load())
        {
            const std::shared_ptr<const uint8[]> held = asset.AcquirePixels();
            const bool whole = held && (std::memcmp(held.get(), firstPixels.data(), firstPixels.size()) == 0 ||
                                        std::memcmp(held.get(), secondPixels.data(), secondPixels.size()) == 0);
            wrongReads.fetch_add(whole ? 0 : 1);
            reads.fetch_add(1);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int failedReloads = 0;
    for (int reload = 0;
         (reload < kReloads || reads.load() < kReads) && std::chrono::steady_clock::now() < deadline; ++reload)
        failedReloads += asset.ReloadFromData(containers[reload % 2]) ? 0 : 1;
    done.store(true);
    reader.join();
    EXPECT_EQ(failedReloads, 0);
    EXPECT_GE(reads.load(), kReads);
    EXPECT_EQ(wrongReads.load(), 0);
}

} // namespace
