#include "PageStreaming/Placement/PackedPlacementStore.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "NonInheritedFile.h"
#include "Types/Fnv1a.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr std::array<char, 4> kMagic{'G', 'E', 'F', 'P'};
constexpr uint32 kVersion = 1;
constexpr uint64 kMaxDirectoryBytes = 64ull * 1024 * 1024;

struct PlacementStoreHeader
{
    std::array<char, 4> Magic;
    uint32 Version;
    uint32 PageCount;
    uint32 PlacementCount;
    uint64 DataOffset;
    uint64 DirectoryHash;
};
static_assert(sizeof(PlacementStoreHeader) == 32);
static_assert(offsetof(PlacementStoreHeader, DataOffset) == 16);
static_assert(offsetof(PlacementStoreHeader, DirectoryHash) == 24);
static_assert(offsetof(PlacementPage, LocalMin) == 24);
static_assert(offsetof(PlacementPage, LocalMax) == 36);
static_assert(offsetof(PlacementPage, ContentHash) == 48);
static_assert(offsetof(PlacementPage, First) == 56);
static_assert(offsetof(PlacementPage, Count) == 60);
static_assert(std::endian::native == std::endian::little);
static_assert(std::numeric_limits<float32>::is_iec559 && std::numeric_limits<double>::is_iec559);

bool ValidPageBounds(const PlacementPage& page)
{
    for (size_t axis = 0; axis < page.Origin.size(); ++axis)
    {
        if (!std::isfinite(page.Origin[axis]) || !std::isfinite(page.LocalMin[axis]) ||
            !std::isfinite(page.LocalMax[axis]) || page.LocalMin[axis] > page.LocalMax[axis] ||
            !std::isfinite(page.Origin[axis] + page.LocalMin[axis]) ||
            !std::isfinite(page.Origin[axis] + page.LocalMax[axis]))
            return false;
    }
    return true;
}

} // namespace

std::string WritePackedPlacementStore(const std::filesystem::path& target, std::span<const PlacementPage> pages,
                                      std::span<const PackedPlacement> placements)
{
    if (pages.empty() || pages.size_bytes() > kMaxDirectoryBytes || placements.size() > std::numeric_limits<uint32>::max())
        return "invalid placement store size";

    std::vector<PlacementPage> directory(pages.begin(), pages.end());
    size_t first = 0;
    for (auto& page : directory)
    {
        if (page.First != first || page.Count == 0 || page.Count > PackedPlacementStore::kPlacementsPerPage ||
            page.Count > placements.size() - first || !ValidPageBounds(page))
            return "invalid placement page " + std::to_string(&page - directory.data());
        const auto payload = placements.subspan(first, page.Count);
        page.ContentHash = Hashing::Fnv1a64(payload.data(), payload.size_bytes());
        first += page.Count;
    }
    if (first != placements.size())
        return "placement pages do not cover the payload";

    const PlacementStoreHeader header{kMagic, kVersion, static_cast<uint32>(pages.size()),
        static_cast<uint32>(placements.size()), sizeof(PlacementStoreHeader) + pages.size_bytes(),
        Hashing::Fnv1a64(directory.data(), pages.size_bytes())};
    std::error_code error;
    if (const auto parent = target.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error)
        return "could not create the placement store directory: " + error.message();

    const auto temp = FileSystem::MakeTemporarySiblingPath(target);
    auto* file = OpenNonInheritedFile(temp, NonInheritedFileMode::CreateWrite);
    if (!file)
        return "could not create the temporary placement store at " + temp.string();
    const bool written = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(directory.data(), sizeof(PlacementPage), directory.size(), file) == directory.size() &&
        std::fwrite(placements.data(), sizeof(PackedPlacement), placements.size(), file) == placements.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        std::filesystem::remove(temp, error);
        return "could not write the placement store at " + temp.string();
    }
    if (!FileSystem::PublishFile(temp, target))
        return "could not publish the placement store at " + target.string();
    return {};
}

std::string PackedPlacementStore::Open(const std::filesystem::path& file)
{
    m_File.clear();
    m_Pages.clear();
    m_DataOffset = 0;
    m_PlacementCount = 0;

    SharedFileReader reader;
    if (!reader.Open(file))
        return "could not open the placement store";
    PlacementStoreHeader header{};
    if (reader.Read(&header, sizeof(header)) != static_cast<int64>(sizeof(header)) ||
        header.Magic != kMagic || header.Version != kVersion)
        return "invalid placement store header";

    const uint64 directoryBytes = static_cast<uint64>(header.PageCount) * sizeof(PlacementPage);
    const uint64 payloadBytes = static_cast<uint64>(header.PlacementCount) * sizeof(PackedPlacement);
    const int64 fileBytes = reader.Size();
    if (header.PageCount == 0 || header.PlacementCount < header.PageCount ||
        directoryBytes > kMaxDirectoryBytes || fileBytes < 0 ||
        header.DataOffset != sizeof(header) + directoryBytes ||
        header.DataOffset + payloadBytes != static_cast<uint64>(fileBytes))
        return "invalid placement store length";

    std::vector<PlacementPage> pages(header.PageCount);
    if (reader.Read(pages.data(), directoryBytes) != static_cast<int64>(directoryBytes) ||
        Hashing::Fnv1a64(pages.data(), directoryBytes) != header.DirectoryHash)
        return "invalid placement page directory";

    uint64 covered = 0;
    for (const PlacementPage& page : pages)
    {
        if (page.First != covered || page.Count == 0 || page.Count > kPlacementsPerPage ||
            !ValidPageBounds(page))
            return "invalid placement page";
        covered += page.Count;
    }
    if (covered != header.PlacementCount)
        return "placement pages do not cover the payload";

    m_File = file;
    m_Pages = std::move(pages);
    m_DataOffset = header.DataOffset;
    m_PlacementCount = header.PlacementCount;
    return {};
}

std::optional<PlacementByteRange> PackedPlacementStore::ReadRange(uint32 page) const
{
    if (page >= m_Pages.size())
        return std::nullopt;
    const auto& entry = m_Pages[page];
    return PlacementByteRange{m_DataOffset + static_cast<uint64>(entry.First) * sizeof(PackedPlacement),
                              static_cast<uint64>(entry.Count) * sizeof(PackedPlacement)};
}

bool PackedPlacementStore::VerifyPage(uint32 page, std::span<const uint8> bytes) const
{
    const auto range = ReadRange(page);
    return range && bytes.size() == range->Bytes &&
        Hashing::Fnv1a64(bytes.data(), bytes.size()) == m_Pages[page].ContentHash;
}

} // namespace GameEngine::PageStreaming
