#include "Assets/PolyhavenThumbnail.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

namespace GameEngine
{

TEST(PolyhavenThumbnailTests, ApiWebpUrlRequestsPngWithoutLosingSizeOrVersion)
{
    EXPECT_EQ(PolyhavenThumbnail::PngUrl(
        "https://cdn.polyhaven.com/asset_img/thumbs/anniversary_lounge.png?width=256&height=256&format=webp&v=b452ac5f"),
        "https://cdn.polyhaven.com/asset_img/thumbs/anniversary_lounge.png?width=256&height=256&v=b452ac5f&format=png");
}

TEST(PolyhavenThumbnailTests, ReplacesEveryFormatParameterAndPreservesFragments)
{
    const auto url = PolyhavenThumbnail::PngUrl("https://cdn.polyhaven.com/thumb.png?format=webp&v=1&format=jpeg#preview");
    EXPECT_EQ(url, "https://cdn.polyhaven.com/thumb.png?v=1&format=png#preview");
    EXPECT_EQ(PolyhavenThumbnail::PngUrl(url), url);
}

TEST(PolyhavenThumbnailTests, AddsFormatWhenTheUrlHasNoQuery)
{
    EXPECT_EQ(PolyhavenThumbnail::PngUrl("https://cdn.polyhaven.com/thumb.png"),
        "https://cdn.polyhaven.com/thumb.png?format=png");
}

TEST(PolyhavenThumbnailTests, DetectsWebpBytesStoredUnderAPngFilename)
{
    EXPECT_FALSE(PolyhavenThumbnail::HasPngSignature(std::string_view("RIFFv*\0\0WEBPVP8 ", 16)));
    EXPECT_FALSE(PolyhavenThumbnail::HasPngSignature("\x89PNG"));
    EXPECT_TRUE(PolyhavenThumbnail::HasPngSignature("\x89PNG\r\n\x1a\n"));
}

TEST(PolyhavenThumbnailTests, CachedHeaderChecksTheBytesRatherThanTheFilename)
{
    TestUtils::ScopedTempDir temp(TestUtils::MakeUniqueTempDirectory("polyhaven-thumbnail-header"));
    const auto path = temp.Path() / "thumbnail.png";
    {
        std::ofstream file(path, std::ios::binary);
        file.write("RIFFv*\0\0WEBPVP8 ", 16);
    }
    EXPECT_FALSE(PolyhavenThumbnail::HasCachedPngHeader(path));
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write("\x89PNG", 4);
    }
    EXPECT_FALSE(PolyhavenThumbnail::HasCachedPngHeader(path));
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write("\x89PNG\r\n\x1a\n", 8);
    }
    EXPECT_TRUE(PolyhavenThumbnail::HasCachedPngHeader(path));
    EXPECT_FALSE(PolyhavenThumbnail::HasCachedPngHeader(temp.Path() / "missing.png"));
}

} // namespace GameEngine
