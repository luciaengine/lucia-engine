#include "Assets/SvgRasterizer.h"
#include "Assets/TextureAsset.h"

#include <string_view>

#include <gtest/gtest.h>

namespace GameEngine
{
namespace
{

TEST(SvgRasterizerTests, ParsesAndClampsPerAssetRasterSize)
{
    uint32 size = 256;
    EXPECT_TRUE(ParseSvgRasterSizeMeta("1024", size));
    EXPECT_EQ(size, 1024u);

    EXPECT_TRUE(ParseSvgRasterSizeMeta("1", size));
    EXPECT_EQ(size, kMinSvgRasterSize);

    EXPECT_TRUE(ParseSvgRasterSizeMeta("999999", size));
    EXPECT_EQ(size, kMaxSvgRasterSize);
}

TEST(SvgRasterizerTests, RejectsMalformedPerAssetRasterSize)
{
    uint32 size = 512;
    EXPECT_FALSE(ParseSvgRasterSizeMeta("", size));
    EXPECT_FALSE(ParseSvgRasterSizeMeta("1024px", size));
    EXPECT_FALSE(ParseSvgRasterSizeMeta("-32", size));
    EXPECT_EQ(size, 512u);
}

TEST(SvgRasterizerTests, KeepsUiAndTextureDefaultsIndependent)
{
    const float32 oldUi = GetSvgRasterizerUserScale();
    const float32 oldTexture = GetSvgTextureRasterizerDefaultSize();

    SetSvgRasterizerUserScale(512.0f);
    SetSvgTextureRasterizerDefaultSize(2048.0f);
    EXPECT_FLOAT_EQ(GetSvgRasterizerUserScale(), 512.0f);
    EXPECT_FLOAT_EQ(GetSvgTextureRasterizerDefaultSize(), 2048.0f);

    SetSvgRasterizerUserScale(oldUi);
    SetSvgTextureRasterizerDefaultSize(oldTexture);
}

TEST(SvgRasterizerTests, CalculatesAspectPreservingRasterDimensions)
{
    uint32 width = 0;
    uint32 height = 0;

    EXPECT_TRUE(CalculateSvgRasterDimensions(20.0f, 10.0f, 80.0f, width, height));
    EXPECT_EQ(width, 80u);
    EXPECT_EQ(height, 40u);

    EXPECT_TRUE(CalculateSvgRasterDimensions(10.0f, 20.0f, 80.0f, width, height));
    EXPECT_EQ(width, 40u);
    EXPECT_EQ(height, 80u);

    EXPECT_TRUE(CalculateSvgRasterDimensions(1000.0f, 333.0f, 512.0f, width, height));
    EXPECT_EQ(width, 512u);
    EXPECT_EQ(height, 170u);

    EXPECT_FALSE(CalculateSvgRasterDimensions(0.0f, 10.0f, 80.0f, width, height));
}

TEST(SvgRasterizerTests, RasterizesToExplicitLongestAxisSize)
{
    constexpr const char* svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">)"
        R"(<rect width="20" height="10" fill="#fff"/></svg>)";

    float32 sourceWidth = 0.0f;
    float32 sourceHeight = 0.0f;
    if (!GetSvgSourceSize(svg, sourceWidth, sourceHeight))
        GTEST_SKIP() << "ThorVG is not available in this build";
    EXPECT_FLOAT_EQ(sourceWidth, 20.0f);
    EXPECT_FLOAT_EQ(sourceHeight, 10.0f);

    SvgRasterizedImage image;
    ASSERT_TRUE(RasterizeSvgToRgbaAtSize(svg, 80.0f, image));

    EXPECT_EQ(image.Width, 80u);
    EXPECT_EQ(image.Height, 40u);
    EXPECT_EQ(image.DataSize, 80u * 40u * 4u);
    EXPECT_NE(image.Data.get(), nullptr);
}

// Project SVGs remain ordinary RGBA textures, independently of the editor's
// icon raster size. Exercise TextureAsset's real in-memory decode entry point.
TEST(SvgRasterizerTests, ProjectSvgLoadsAsTextureUsingTheTextureImportSize)
{
    constexpr std::string_view svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">)"
        R"(<rect width="20" height="10" fill="#123456"/></svg>)";
    float32 sourceWidth = 0.0f, sourceHeight = 0.0f;
    if (!GetSvgSourceSize(std::string(svg), sourceWidth, sourceHeight))
        GTEST_SKIP() << "ThorVG is not available in this build";

    struct RestoreRasterSizes
    {
        float32 Ui = GetSvgRasterizerUserScale();
        float32 Texture = GetSvgTextureRasterizerDefaultSize();
        ~RestoreRasterSizes()
        {
            SetSvgRasterizerUserScale(Ui);
            SetSvgTextureRasterizerDefaultSize(Texture);
        }
    } restore;
    SetSvgRasterizerUserScale(512.0f);
    SetSvgTextureRasterizerDefaultSize(80.0f);
    TextureAsset texture(GUID::Null(), "ProjectSvgTexture.svg");
    const Vector<uint8> bytes(svg.begin(), svg.end());
    ASSERT_TRUE(texture.LoadFromData(bytes));
    EXPECT_EQ(texture.GetWidth(), 80u);
    EXPECT_EQ(texture.GetHeight(), 40u);
    EXPECT_EQ(texture.GetFormat(), TextureFormat::RGBA8);
    ASSERT_NE(texture.GetPixelData(), nullptr);
    const uint8* center = texture.GetPixelData() + (20u * texture.GetWidth() + 40u) * 4u;
    EXPECT_EQ(center[0], 0x12u);
    EXPECT_EQ(center[1], 0x34u);
    EXPECT_EQ(center[2], 0x56u);
    EXPECT_EQ(center[3], 0xFFu);
}

} // namespace
} // namespace GameEngine
