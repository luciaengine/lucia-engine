#include "PackedPlacementTestArchive.h"

#include "AssetCore/SharedFileRead.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::array<PackedPlacement, 3> TestPlacements()
{
    std::array<PackedPlacement, 3> placements{};
    std::memcpy(placements.data(), kFirstPage.data(), kFirstPage.size());
    std::memcpy(placements.data() + 1, kSecondPage.data(), kSecondPage.size());
    return placements;
}

class PackedPlacementStoreTest : public PackedPlacementArchiveTest
{
protected:
    const std::filesystem::path m_Valid = m_Scratch.Path() / "valid.geforest";
    const std::filesystem::path m_Invalid = m_Scratch.Path() / "invalid.geforest";
    const std::array<PlacementPage, 2> m_ValidPages = m_Pages;
    PackedPlacementStore m_Store;

    // Writes the current fixture state as the invalid store and restores the valid state. A failed
    // open after a successful one must clear the store.
    void ExpectRejected(bool corruptDirectory = false)
    {
        WriteStore(m_Invalid, corruptDirectory);
        RestoreValidArchive();
        ASSERT_TRUE(m_Store.Open(m_Valid).empty());
        EXPECT_FALSE(m_Store.Open(m_Invalid).empty());
        EXPECT_TRUE(m_Store.Pages().empty());
        EXPECT_TRUE(m_Store.File().empty());
        EXPECT_EQ(m_Store.BoundRadius(), 0.0f);
        EXPECT_FALSE(m_Store.ReadRange(0));
    }

    void RestoreValidArchive()
    {
        m_Pages = m_ValidPages;
        m_Magic = {'G', 'E', 'F', 'P'};
        m_Version = 1;
        m_PlacementCount = 3;
        m_BoundRadius = kTestBoundRadius;
        m_Padding = 0;
        m_DataOffset = kDataOffset;
        m_TrailingBytes = 0;
        WriteStore(m_Valid);
    }
};

TEST_F(PackedPlacementStoreTest, WritesTheCanonicalArchiveAndKeepsItAfterARejectedCook)
{
    const auto file = m_Scratch.Path() / "written.geforest";
    const auto placements = TestPlacements();
    auto pages = m_Pages;
    for (auto& page : pages)
        page.ContentHash = 0;
    ASSERT_TRUE(WritePackedPlacementStore(file, kTestBoundRadius, pages, placements).empty());

    PackedPlacementStore store;
    ASSERT_TRUE(store.Open(file).empty());
    EXPECT_TRUE(store.VerifyPage(0, kFirstPage));
    EXPECT_TRUE(store.VerifyPage(1, kSecondPage));
    EXPECT_EQ(store.BoundRadius(), kTestBoundRadius);
    const auto expected = m_Scratch.Path() / "reference.geforest";
    WriteStore(expected);
    ASSERT_EQ(ReadBytes(file), ReadBytes(expected));

    ++pages[1].First;
    EXPECT_FALSE(WritePackedPlacementStore(file, kTestBoundRadius, pages, placements).empty());
    --pages[1].First;
    pages[0].LocalMax.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(WritePackedPlacementStore(file, kTestBoundRadius, pages, placements).empty());
    pages[0].LocalMax = m_Pages[0].LocalMax;
    EXPECT_FALSE(WritePackedPlacementStore(file, 0.0f, pages, placements).empty());
    EXPECT_EQ(ReadBytes(file), ReadBytes(expected));
}

TEST_F(PackedPlacementStoreTest, WriterRefusesMorePagesThanTheDirectoryLimit)
{
    // One placement per page, so that only the page count is wrong.
    std::vector<PlacementPage> pages(PackedPlacementStore::kMaxPages + 1, m_Pages[0]);
    for (uint32 page = 0; page < pages.size(); ++page)
        pages[page].First = page;
    const std::vector<PackedPlacement> placements(pages.size(), TestPlacements()[0]);
    const auto file = m_Scratch.Path() / "too-many.geforest";
    EXPECT_FALSE(WritePackedPlacementStore(file, kTestBoundRadius, pages, placements).empty());
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(PackedPlacementStoreTest, ResolvesVariablePageRangesAndVerifiesPayload)
{
    m_BoundRadius = 7.5f;
    WriteStore(m_Valid);
    ASSERT_TRUE(m_Store.Open(m_Valid).empty());
    EXPECT_EQ(m_Store.File(), m_Valid);
    EXPECT_EQ(m_Store.BoundRadius(), 7.5f);
    ASSERT_EQ(m_Store.Pages().size(), 2u);
    EXPECT_EQ(m_Store.Pages()[1].Origin.X, m_Pages[1].Origin.X);
    EXPECT_EQ(m_Store.Pages()[1].Origin.Z, m_Pages[1].Origin.Z);
    EXPECT_EQ(m_Store.Pages()[1].LocalMax.y, m_Pages[1].LocalMax.y);
    const auto first = m_Store.ReadRange(0);
    const auto second = m_Store.ReadRange(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->Offset, kDataOffset);
    EXPECT_EQ(first->Bytes, kFirstPage.size());
    EXPECT_EQ(second->Offset, kDataOffset + kFirstPage.size());
    EXPECT_EQ(second->Bytes, kSecondPage.size());
    EXPECT_FALSE(m_Store.ReadRange(2));
    EXPECT_FALSE(m_Store.VerifyPage(2, {}));
    EXPECT_FALSE(m_Store.VerifyPage(0, kSecondPage));

    SharedFileReader reader;
    ASSERT_TRUE(reader.Open(m_Valid));
    ASSERT_TRUE(reader.SeekTo(second->Offset));
    std::array<uint8, kSecondPage.size()> bytes{};
    ASSERT_EQ(reader.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    EXPECT_TRUE(m_Store.VerifyPage(1, bytes));
    bytes[5] ^= 1;
    EXPECT_FALSE(m_Store.VerifyPage(1, bytes));
}

TEST_F(PackedPlacementStoreTest, VerifyPageRequiresTheWholeRange)
{
    // The hash of a prefix of page 1 does not verify that prefix as the page.
    const auto prefix = std::span(kSecondPage).first(sizeof(PackedPlacement));
    m_Pages[1].ContentHash = Hashing::Fnv1a64(prefix.data(), prefix.size());
    WriteStore(m_Valid);
    ASSERT_TRUE(m_Store.Open(m_Valid).empty());
    EXPECT_FALSE(m_Store.VerifyPage(1, prefix));
}

TEST_F(PackedPlacementStoreTest, RejectsMalformedDirectoriesAndClearsPreviousState)
{
    ExpectRejected(true);
    m_Pages[1].First = 0;
    ExpectRejected();
    m_Pages[1].Count = PackedPlacementStore::kPlacementsPerPage + 1;
    ExpectRejected();
    m_Pages[1].Origin.Z = std::numeric_limits<double>::infinity();
    ExpectRejected();
    m_Pages[1].LocalMin.z = m_Pages[1].LocalMax.z + 1;
    ExpectRejected();
    // Page 0 covers every placement, so only the empty page 1 is wrong.
    m_Pages[0].Count = 3;
    m_Pages[1].First = 3;
    m_Pages[1].Count = 0;
    ExpectRejected();
}

TEST_F(PackedPlacementStoreTest, RejectsMalformedHeadersAndLengths)
{
    m_Magic = {'G', 'E', 'F', 'Q'};
    ExpectRejected();
    m_Version = 2;
    ExpectRejected();
    m_BoundRadius = 0.0f;
    ExpectRejected();
    m_BoundRadius = std::numeric_limits<float>::quiet_NaN();
    ExpectRejected();
    m_Padding = 1;
    ExpectRejected();
    m_TrailingBytes = 1;
    ExpectRejected();
    // A payload offset past the directory, with the file padded to match the payload length.
    m_DataOffset = kDataOffset + sizeof(PackedPlacement);
    m_TrailingBytes = sizeof(PackedPlacement);
    ExpectRejected();
    WriteStore(m_Invalid);
    std::filesystem::resize_file(m_Invalid, kDataOffset + 1);
    RestoreValidArchive();
    ASSERT_TRUE(m_Store.Open(m_Valid).empty());
    EXPECT_FALSE(m_Store.Open(m_Invalid).empty());
    EXPECT_TRUE(m_Store.Pages().empty());
}

} // namespace
} // namespace GameEngine::PageStreaming
