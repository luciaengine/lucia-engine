#include "PageStreaming/Placement/PackedPlacementStore.h"

#include "AssetCore/SharedFileRead.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr uint64 kHeaderBytes = 32;
constexpr uint64 kDirectoryBytes = 2 * sizeof(PlacementPage);
constexpr uint64 kDataOffset = kHeaderBytes + kDirectoryBytes;
constexpr std::array<uint8, 8> kFirstPage{12, 0, 99, 0, 0, 0, 0, 128};
constexpr std::array<uint8, 16> kSecondPage{
    16, 0, 30, 0, 23, 0, 255, 0,
    64, 1, 88, 2, 255, 255, 255, 255,
};

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Opens a valid store, then checks that a failed open of the invalid store clears it.
void ExpectRejected(PackedPlacementStore& store, const std::filesystem::path& valid, const std::filesystem::path& invalid)
{
    ASSERT_TRUE(store.Open(valid).empty());
    EXPECT_FALSE(store.Open(invalid).empty());
    EXPECT_TRUE(store.Pages().empty());
    EXPECT_TRUE(store.File().empty());
    EXPECT_EQ(store.PlacementCount(), 0u);
    EXPECT_FALSE(store.ReadRange(0));
}

template<class T>
void Write(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

class PackedPlacementStoreTest : public ::testing::Test
{
protected:
    TestUtils::ScopedTempDir m_Scratch{TestUtils::MakeUniqueTempDirectory("PackedPlacementStore")};
    std::array<PlacementPage, 2> m_Pages{
        PlacementPage{{512'000'000.0, 8.0, 512'000'001.0}, Mathematics::Vector3(-2.0f, 0.0f, -2.0f), Mathematics::Vector3(18.0f, 25.0f, 18.0f),
                      Hashing::Fnv1a64(kFirstPage.data(), kFirstPage.size()), 0, 1},
        PlacementPage{{512'000'055.0, 8.0, 512'000'002.0}, Mathematics::Vector3(-2.0f, 0.0f, -2.0f), Mathematics::Vector3(18.0f, 25.0f, 18.0f),
                      Hashing::Fnv1a64(kSecondPage.data(), kSecondPage.size()), 1, 2},
    };

    void WriteStore(const std::filesystem::path& path, bool corruptDirectory = false)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        Write(out, std::array<char, 4>{'G', 'E', 'F', 'P'});
        Write(out, uint32{1});
        Write(out, uint32{2});
        Write(out, uint32{3});
        Write(out, kDataOffset);
        const uint64 hash = Hashing::Fnv1a64(m_Pages.data(), kDirectoryBytes);
        Write(out, corruptDirectory ? hash ^ 1 : hash);
        Write(out, m_Pages);
        Write(out, kFirstPage);
        Write(out, kSecondPage);
        out.close();
        ASSERT_TRUE(out);
    }
};


TEST_F(PackedPlacementStoreTest, WritesTheCanonicalArchiveAndKeepsItAfterARejectedCook)
{
    const auto file = m_Scratch.Path() / "written.geforest";
    std::array<PackedPlacement, 3> placements{};
    std::memcpy(placements.data(), kFirstPage.data(), kFirstPage.size());
    std::memcpy(placements.data() + 1, kSecondPage.data(), kSecondPage.size());
    auto pages = m_Pages;
    for (auto& page : pages)
        page.ContentHash = 0;
    ASSERT_TRUE(WritePackedPlacementStore(file, pages, placements).empty());

    PackedPlacementStore store;
    ASSERT_TRUE(store.Open(file).empty());
    EXPECT_TRUE(store.VerifyPage(0, kFirstPage));
    EXPECT_TRUE(store.VerifyPage(1, kSecondPage));
    const auto expected = m_Scratch.Path() / "reference.geforest";
    WriteStore(expected);
    ASSERT_EQ(ReadBytes(file), ReadBytes(expected));

    ++pages[1].First;
    EXPECT_FALSE(WritePackedPlacementStore(file, pages, placements).empty());
    EXPECT_EQ(ReadBytes(file), ReadBytes(expected));
}

TEST_F(PackedPlacementStoreTest, ResolvesVariablePageRangesAndVerifiesPayload)
{
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    PackedPlacementStore store;
    ASSERT_TRUE(store.Open(file).empty());
    EXPECT_EQ(store.File(), file);
    EXPECT_EQ(store.PlacementCount(), 3u);
    ASSERT_EQ(store.Pages().size(), 2u);
    EXPECT_EQ(store.Pages()[1].Origin.X, m_Pages[1].Origin.X);
    EXPECT_EQ(store.Pages()[1].Origin.Z, m_Pages[1].Origin.Z);
    EXPECT_EQ(store.Pages()[1].LocalMax.y, m_Pages[1].LocalMax.y);
    const auto first = store.ReadRange(0);
    const auto second = store.ReadRange(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->Offset, kDataOffset);
    EXPECT_EQ(first->Bytes, kFirstPage.size());
    EXPECT_EQ(second->Offset, kDataOffset + kFirstPage.size());
    EXPECT_EQ(second->Bytes, kSecondPage.size());
    EXPECT_FALSE(store.ReadRange(2));
    EXPECT_FALSE(store.VerifyPage(2, {}));
    EXPECT_FALSE(store.VerifyPage(0, kSecondPage));

    SharedFileReader reader;
    ASSERT_TRUE(reader.Open(file));
    ASSERT_TRUE(reader.SeekTo(second->Offset));
    std::array<uint8, kSecondPage.size()> bytes{};
    ASSERT_EQ(reader.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    EXPECT_TRUE(store.VerifyPage(1, bytes));
    bytes[5] ^= 1;
    EXPECT_FALSE(store.VerifyPage(1, bytes));
}

TEST_F(PackedPlacementStoreTest, RejectsMalformedDirectoriesAndClearsPreviousState)
{
    const auto valid = m_Scratch.Path() / "valid.geforest";
    const auto invalid = m_Scratch.Path() / "invalid.geforest";
    WriteStore(valid);
    PackedPlacementStore store;

    WriteStore(invalid, true);
    ExpectRejected(store, valid, invalid);
    const auto original = m_Pages[1];
    m_Pages[1].First = 0;
    WriteStore(invalid);
    ExpectRejected(store, valid, invalid);
    m_Pages[1] = original;
    m_Pages[1].Count = PackedPlacementStore::kPlacementsPerPage + 1;
    WriteStore(invalid);
    ExpectRejected(store, valid, invalid);
    m_Pages[1] = original;
    m_Pages[1].Origin.Z = std::numeric_limits<double>::infinity();
    WriteStore(invalid);
    ExpectRejected(store, valid, invalid);
    m_Pages[1] = original;
    m_Pages[1].LocalMin.z = m_Pages[1].LocalMax.z + 1;
    WriteStore(invalid);
    ExpectRejected(store, valid, invalid);
    m_Pages[1] = original;
    WriteStore(invalid);
    std::filesystem::resize_file(invalid, kDataOffset + 1);
    ExpectRejected(store, valid, invalid);
    ASSERT_TRUE(store.Open(valid).empty());
    EXPECT_EQ(store.PlacementCount(), 3u);
}


} // namespace
} // namespace GameEngine::PageStreaming
