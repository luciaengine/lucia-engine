#pragma once

#include "PageStreaming/Placement/PackedPlacementStore.h"

#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

namespace GameEngine::PageStreaming
{

// A two-page archive: page 0 holds one placement, page 1 holds two.
constexpr uint64 kHeaderBytes = 40;
constexpr uint64 kDirectoryBytes = 2 * sizeof(PlacementPage);
constexpr uint64 kDataOffset = kHeaderBytes + kDirectoryBytes;
constexpr float kTestBoundRadius = 12.0f;
constexpr std::array<uint8, 8> kFirstPage{12, 0, 99, 0, 0, 0, 0, 128};
constexpr std::array<uint8, 16> kSecondPage{
    16, 0, 30, 0, 23, 0, 255, 0,
    64, 1, 88, 2, 0, 0, 255, 255,
};

inline const Mathematics::Vector3 kTestPageMin(-2.0f, 0.0f, -2.0f);
inline const Mathematics::Vector3 kTestPageMax(18.0f, 25.0f, 18.0f);

template<class T>
void WriteValue(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

class PackedPlacementArchiveTest : public ::testing::Test
{
protected:
    TestUtils::ScopedTempDir m_Scratch{TestUtils::MakeUniqueTempDirectory("PackedPlacementArchive")};

    std::array<PlacementPage, 2> m_Pages{
        PlacementPage{{512'000'000.0, 8.0, 512'000'001.0}, kTestPageMin, kTestPageMax,
                      Hashing::Fnv1a64(kFirstPage.data(), kFirstPage.size()), 0, 1},
        PlacementPage{{512'000'055.0, 8.0, 512'000'002.0}, kTestPageMin, kTestPageMax,
                      Hashing::Fnv1a64(kSecondPage.data(), kSecondPage.size()), 1, 2},
    };
    // Header fields a test may change before WriteStore.
    std::array<char, 4> m_Magic{'G', 'E', 'F', 'P'};
    uint32 m_Version = 1;
    uint32 m_PlacementCount = 3;
    float m_BoundRadius = kTestBoundRadius;
    uint32 m_Padding = 0;
    uint64 m_DataOffset = kDataOffset;
    uint32 m_TrailingBytes = 0;

    void WriteStore(const std::filesystem::path& path, bool corruptDirectory = false)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        WriteValue(out, m_Magic);
        WriteValue(out, m_Version);
        WriteValue(out, uint32{2});
        WriteValue(out, m_PlacementCount);
        WriteValue(out, m_BoundRadius);
        WriteValue(out, m_Padding);
        WriteValue(out, m_DataOffset);
        const uint64 hash = Hashing::Fnv1a64(m_Pages.data(), kDirectoryBytes);
        WriteValue(out, corruptDirectory ? hash ^ 1 : hash);
        WriteValue(out, m_Pages);
        WriteValue(out, kFirstPage);
        WriteValue(out, kSecondPage);
        for (uint32 byte = 0; byte < m_TrailingBytes; ++byte)
            WriteValue(out, uint8{0});
        out.close();
        ASSERT_TRUE(out);
    }
};

} // namespace GameEngine::PageStreaming
