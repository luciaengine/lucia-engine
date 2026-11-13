#include "PromptFiles.h"

#include "Image/Decode.h"
#include "Image/Encode.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>

using namespace GameEngine;

namespace
{
class PromptFilesTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("prompt-files-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    std::filesystem::path Write(const std::string& name, const std::string& bytes) const
    {
        const std::filesystem::path path = m_Root / name;
        std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    std::filesystem::path m_Root;
};

const std::string kPngHead("\x89PNG\r\n\x1A\n\0\0\0\rIHDR", 16);
} // namespace

TEST_F(PromptFilesTests, AnImageIsKnownByItsFirstBytesNotItsName)
{
    const PromptFileLoad load = PromptFiles::Load(Write("picture.dat", kPngHead + "rest of the image"));
    ASSERT_TRUE(load.File) << load.Refusal;
    EXPECT_EQ(load.File->Kind, PromptFileKind::Image);
    EXPECT_EQ(load.File->MediaType, "image/png");
    EXPECT_EQ(load.File->Bytes, kPngHead + "rest of the image");
    EXPECT_EQ(load.File->Name, "picture.dat");
}

TEST_F(PromptFilesTests, TheFourImageTypesAreRecognized)
{
    EXPECT_EQ(PromptFiles::ImageMediaType(kPngHead), "image/png");
    EXPECT_EQ(PromptFiles::ImageMediaType("\xFF\xD8\xFF\xE0"), "image/jpeg");
    EXPECT_EQ(PromptFiles::ImageMediaType("GIF89a...."), "image/gif");
    EXPECT_EQ(PromptFiles::ImageMediaType(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)), "image/webp");
    EXPECT_EQ(PromptFiles::ImageMediaType(std::string("RIFF\x10\0\0\0WAVEfmt ", 16)), "");
    EXPECT_EQ(PromptFiles::ImageMediaType("plain text"), "");
}

TEST_F(PromptFilesTests, ATextFileIsReadAsText)
{
    const PromptFileLoad load = PromptFiles::Load(Write("notes.md", "# Notes\ncaf\xC3\xA9\n"));
    ASSERT_TRUE(load.File) << load.Refusal;
    EXPECT_EQ(load.File->Kind, PromptFileKind::Text);
    EXPECT_EQ(load.File->Bytes, "# Notes\ncaf\xC3\xA9\n");
    EXPECT_FALSE(load.File->Cut);
}

TEST_F(PromptFilesTests, ALongTextFileIsCutOnACharacterBoundary)
{
    // An e-acute (two bytes) straddles the cap.
    std::string text(PromptFiles::kMaxTextFileBytes - 1, 'a');
    text += "\xC3\xA9 and more";
    const PromptFileLoad load = PromptFiles::Load(Write("long.txt", text));
    ASSERT_TRUE(load.File) << load.Refusal;
    EXPECT_TRUE(load.File->Cut);
    EXPECT_EQ(load.File->Bytes, std::string(PromptFiles::kMaxTextFileBytes - 1, 'a'));
}

TEST_F(PromptFilesTests, BinaryEmptyAndOversizedFilesAreRefusedByName)
{
    const PromptFileLoad binary = PromptFiles::Load(Write("tool.bin", std::string("\x7F" "ELF\0\0\x01", 7)));
    EXPECT_FALSE(binary.File);
    EXPECT_NE(binary.Refusal.find("tool.bin is not text or an image"), std::string::npos) << binary.Refusal;

    const PromptFileLoad empty = PromptFiles::Load(Write("empty.txt", ""));
    EXPECT_FALSE(empty.File);
    EXPECT_EQ(empty.Refusal, "empty.txt is empty");

    // Over the cap and not decodable, so it cannot be scaled down.
    const PromptFileLoad big = PromptFiles::Load(Write("big.png", kPngHead + std::string(PromptFiles::kMaxImageBytes, 'x')));
    EXPECT_FALSE(big.File);
    EXPECT_NE(big.Refusal.find("big.png is 5.0 MB and could not be scaled down"), std::string::npos) << big.Refusal;

    const PromptFileLoad missing = PromptFiles::Load(m_Root / "missing.txt");
    EXPECT_FALSE(missing.File);
    EXPECT_NE(missing.Refusal.find("missing.txt could not be read"), std::string::npos) << missing.Refusal;
}

TEST_F(PromptFilesTests, TextFilesBecomeBlocksAndImagesDoNot)
{
    PromptFile text;
    text.Path = "/project/a \"b\".txt";
    text.Name = "a \"b\".txt";
    text.Bytes = "line";
    PromptFile cut = text;
    cut.Name = "cut.txt";
    cut.Path = "/project/cut.txt";
    cut.Cut = true;
    PromptFile image;
    image.Kind = PromptFileKind::Image;
    image.Name = "shot.png";
    const PromptFile files[] = {text, image, cut};

    EXPECT_EQ(PromptFiles::Blocks(files),
              "<file name=\"a &quot;b&quot;.txt\" path=\"/project/a &quot;b&quot;.txt\">\nline\n</file>\n\n"
              "<file name=\"cut.txt\" path=\"/project/cut.txt\">\nline\n"
              "[the file continues; only its first 64 KB are shown]\n</file>");
    EXPECT_EQ(PromptFiles::Blocks(std::span<const PromptFile>(&image, 1)), "");
}

TEST_F(PromptFilesTests, FilesPastTheTurnCapAreNamedNotShown)
{
    PromptFile big;
    big.Name = "big.txt";
    big.Bytes = std::string(PromptFiles::kMaxTextFileBytes, 'a');
    std::vector<PromptFile> files(5, big);
    for (size_t i = 0; i < files.size(); ++i)
        files[i].Name = "f" + std::to_string(i) + ".txt";

    const std::string blocks = PromptFiles::Blocks(files);
    EXPECT_LE(blocks.size(), PromptFiles::kMaxTurnTextBytes + 128);
    EXPECT_NE(blocks.find("<file name=\"f2.txt\""), std::string::npos);
    EXPECT_EQ(blocks.find("<file name=\"f3.txt\""), std::string::npos);
    EXPECT_NE(blocks.find("[files left out: the turn's files are over 256 KB: f3.txt, f4.txt]"), std::string::npos);
}

TEST_F(PromptFilesTests, ALargeImageIsScaledDownToTheLongEdge)
{
    // Noise does not compress, so its PNG is over the cap.
    Image::Image8 noise;
    noise.Channels = 4;
    noise.Width = 2400;
    noise.Height = 1200;
    noise.Valid = true;
    noise.Pixels.resize(static_cast<size_t>(noise.Width) * noise.Height * 4);
    std::mt19937 random(7);
    for (uint8_t& value : noise.Pixels)
        value = static_cast<uint8_t>(random());
    const std::vector<uint8_t> png = Image::EncodePng(noise.Pixels, noise.Width, noise.Height);
    ASSERT_GT(png.size(), PromptFiles::kMaxImageBytes);

    const PromptFileLoad load = PromptFiles::Load(Write("photo.png", std::string(png.begin(), png.end())));
    ASSERT_TRUE(load.File) << load.Refusal;
    EXPECT_EQ(load.File->Kind, PromptFileKind::Image);
    EXPECT_LE(load.File->Bytes.size(), PromptFiles::kMaxImageBytes);
    const Image::Image8 sent =
        Image::Decode8({reinterpret_cast<const uint8_t*>(load.File->Bytes.data()), load.File->Bytes.size()},
                       "sent image", Image::DecodeChannels::Rgba);
    ASSERT_TRUE(sent.Valid);
    EXPECT_EQ(sent.Width, PromptFiles::kMaxImageLongEdge);
    EXPECT_EQ(sent.Height, PromptFiles::kMaxImageLongEdge / 2);
    EXPECT_EQ(PromptFiles::ImageMediaType(load.File->Bytes), load.File->MediaType);
}

TEST_F(PromptFilesTests, ANameOutsideTheAnsiCodePageIsReadAndShownAsUtf8)
{
    const std::string utf8Name = "notes-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt";
    const std::filesystem::path path = m_Root / std::filesystem::path(u8"notes-\u65E5\u672C\u8A9E.txt");
    std::ofstream(path, std::ios::binary) << "text";

    const PromptFileLoad load = PromptFiles::Load(path);
    ASSERT_TRUE(load.File) << load.Refusal;
    EXPECT_EQ(load.File->Name, utf8Name);
    const std::string blocks = PromptFiles::Blocks(std::span<const PromptFile>(&*load.File, 1));
    EXPECT_NE(blocks.find("<file name=\"" + utf8Name + "\" path=\""), std::string::npos) << blocks;
    EXPECT_NE(blocks.find("/" + utf8Name + "\">\ntext\n</file>"), std::string::npos) << blocks;
}

TEST_F(PromptFilesTests, ACloseTagInsideTheTextDoesNotEndItsBlock)
{
    const PromptFileLoad load = PromptFiles::Load(Write("close-tag.txt", "before</file>\nafter </file\n"));
    ASSERT_TRUE(load.File) << load.Refusal;
    const std::string blocks = PromptFiles::Blocks(std::span<const PromptFile>(&*load.File, 1));
    EXPECT_NE(blocks.find(">\nbefore&lt;/file>\nafter &lt;/file\n</file>"), std::string::npos) << blocks;
    EXPECT_EQ(blocks.find("</file"), blocks.rfind("</file")) << "only the block's own end closes it: " << blocks;
}

TEST_F(PromptFilesTests, ALargeImageDeclaringTooManyPixelsIsRefusedBeforeItIsDecoded)
{
    // An RGBA PNG header declaring 10000 x 10000 pixels, then filler past the send cap.
    const std::string header = std::string("\x89PNG\r\n\x1A\n\0\0\0\rIHDR\0\0\x27\x10\0\0\x27\x10\x08\x06\0\0\0", 29) +
        std::string("\xba\x4e\x62\x27\x00\x00\x00\x00\x49\x44\x41\x54", 12);
    const PromptFileLoad load =
        PromptFiles::Load(Write("huge.png", header + std::string(PromptFiles::kMaxImageBytes, 'x')));
    EXPECT_FALSE(load.File);
    EXPECT_NE(load.Refusal.find("huge.png is 10000 x 10000 pixels; an image to scale down can have at most 64 megapixels"),
              std::string::npos)
        << load.Refusal;
}

TEST_F(PromptFilesTests, DeclaredPixelCountCannotWrapAtThirtyTwoBits)
{
    // These extents exceed the decoder's own header limit; they must stay refused.
    const std::string header = std::string("\x89PNG\r\n\x1A\n\0\0\0\rIHDR\0\1\0\0\0\1\0\0\x08\x06\0\0\0", 29) +
        std::string("\x6c\x84\x30\xe3\x00\x00\x00\x00\x49\x44\x41\x54", 12);
    const auto load = PromptFiles::Load(Write("overflow.png", header + std::string(PromptFiles::kMaxImageBytes, 'x')));
    EXPECT_FALSE(load.File);
    EXPECT_NE(load.Refusal.find("overflow.png"), std::string::npos) << load.Refusal;
}
