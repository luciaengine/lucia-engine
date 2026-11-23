#include "ForestOverviewCook.h"

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/ParseNumber.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
using PageStreaming::PackedPlacement;
using PageStreaming::PackedPlacementStore;
using PageStreaming::PlacementPage;

struct Tree
{
    double X;
    double Y;
    double Z;
    double Yaw;
    double Scale;
};

constexpr std::string_view kCsvHeader = "page,x,y,z,yawDegrees,scale";
constexpr size_t kCsvFields = 6;
constexpr double kCentimetersPerMeter = 100.0;
constexpr double kDegreesPerTurn = 360.0;
constexpr uint16 kLargestOffset = std::numeric_limits<uint16>::max();
constexpr uint8 kLargestScale = std::numeric_limits<uint8>::max();
// The largest world coordinate in meters; WorldPosition splits it into int32 sectors.
constexpr double kMaxWorldCoordinate = 1.0e12;
// A packed offset may round up by half a centimeter.
constexpr float kRoundingMargin = 0.005f;

std::string_view Field(std::string_view text)
{
    return TrimWhitespaceView(text);
}

bool InWorld(double coordinate)
{
    return std::abs(coordinate) <= kMaxWorldCoordinate;
}

// Parses one CSV row. Every field may have spaces around it.
bool ParseTree(std::string_view line, int64& page, Tree& tree)
{
    const std::vector<std::string_view> fields = Split(line, ',');
    if (fields.size() != kCsvFields)
        return false;
    const auto pageId = ParseInt(Field(fields[0]));
    if (!pageId || *pageId < 0)
        return false;
    page = *pageId;
    return ParseStrictDecimal(Field(fields[1]), tree.X) && ParseStrictDecimal(Field(fields[2]), tree.Y) &&
           ParseStrictDecimal(Field(fields[3]), tree.Z) && ParseStrictDecimal(Field(fields[4]), tree.Yaw) &&
           ParseStrictDecimal(Field(fields[5]), tree.Scale) && InWorld(tree.X) && InWorld(tree.Y) &&
           InWorld(tree.Z) && std::isfinite(tree.Yaw) && tree.Scale >= PackedPlacement::kMinScale &&
           tree.Scale <= PackedPlacement::kMaxScale;
}

uint16 PackOffset(double meters, const std::string& label)
{
    const double centimeters = meters * kCentimetersPerMeter;
    if (centimeters >= kLargestOffset + 0.5)
        throw std::runtime_error(label + " spans more than 655.35 meters on one axis. Split it into smaller pages.");
    return static_cast<uint16>(std::lround(centimeters));
}

uint8 PackYaw(double degrees)
{
    const double turn = std::fmod(degrees, kDegreesPerTurn) / kDegreesPerTurn;
    return static_cast<uint8>(static_cast<int64>(std::lround(turn * PackedPlacement::kYawSteps)) & 0xFF);
}

uint8 PackScale(double scale)
{
    constexpr double kRange = PackedPlacement::kMaxScale - PackedPlacement::kMinScale;
    return static_cast<uint8>(std::lround((scale - PackedPlacement::kMinScale) * (kLargestScale / kRange)));
}

void AppendPage(std::span<const Tree> trees, float boundRadius, std::vector<PlacementPage>& pages,
                std::vector<PackedPlacement>& placements)
{
    const std::string label = "Page " + std::to_string(pages.size());
    double x = trees.front().X;
    double y = trees.front().Y;
    double z = trees.front().Z;
    for (const Tree& tree : trees)
    {
        x = std::min(x, tree.X);
        y = std::min(y, tree.Y);
        z = std::min(z, tree.Z);
    }

    PlacementPage page{{x, y, z}, {}, {}, 0, static_cast<uint32>(placements.size()), static_cast<uint32>(trees.size())};
    // Use the largest scale so that rounding and the shader decode cannot shrink the bounds.
    const float radius =
        std::nextafter(boundRadius * PackedPlacement::kMaxScale, std::numeric_limits<float>::infinity());
    page.LocalMin = Mathematics::Vector3(-radius, -radius, -radius);
    page.LocalMax = Mathematics::Vector3(radius, radius, radius);
    for (const Tree& tree : trees)
    {
        const PackedPlacement packed{PackOffset(tree.X - x, label), PackOffset(tree.Z - z, label),
                                     PackOffset(tree.Y - y, label), PackYaw(tree.Yaw), PackScale(tree.Scale)};
        placements.push_back(packed);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const double local = axis == 0 ? packed.LocalX() : axis == 1 ? packed.LocalY() : packed.LocalZ();
            page.LocalMax[axis] = std::max(page.LocalMax[axis], static_cast<float>(local) + radius);
        }
    }
    for (size_t axis = 0; axis < 3; ++axis)
        page.LocalMax[axis] =
            std::nextafter(page.LocalMax[axis] + kRoundingMargin, std::numeric_limits<float>::infinity());
    pages.push_back(page);
}

uint32 QuantizePageCenter(double center, double min, double max)
{
    if (min == max)
        return 0;
    return static_cast<uint32>(std::clamp((center - min) / (max - min), 0.0, 1.0) * kLargestOffset);
}

void OrderPagesSpatially(std::vector<PlacementPage>& pages, std::vector<PackedPlacement>& placements)
{
    if (pages.size() < 2)
        return;
    // The center of each page's bounds on the ground plane.
    std::vector<std::pair<double, double>> centers;
    centers.reserve(pages.size());
    for (const auto& page : pages)
        centers.emplace_back(std::midpoint(page.Origin.X + page.LocalMin.x, page.Origin.X + page.LocalMax.x),
                             std::midpoint(page.Origin.Z + page.LocalMin.z, page.Origin.Z + page.LocalMax.z));
    auto [minX, maxX] = std::ranges::minmax(centers | std::views::keys);
    auto [minZ, maxZ] = std::ranges::minmax(centers | std::views::values);

    std::vector<std::pair<uint64, size_t>> order;
    order.reserve(pages.size());
    for (size_t page = 0; page < pages.size(); ++page)
        order.emplace_back(PageStreaming::PageMortonCode(QuantizePageCenter(centers[page].first, minX, maxX),
                                                         QuantizePageCenter(centers[page].second, minZ, maxZ)),
                           page);
    std::ranges::sort(order);
    std::vector<PlacementPage> sortedPages;
    std::vector<PackedPlacement> sortedPlacements;
    sortedPages.reserve(pages.size());
    sortedPlacements.reserve(placements.size());
    for (const auto& entry : order)
    {
        auto page = pages[entry.second];
        const auto payload = std::span(placements).subspan(page.First, page.Count);
        page.First = static_cast<uint32>(sortedPlacements.size());
        sortedPlacements.insert(sortedPlacements.end(), payload.begin(), payload.end());
        sortedPages.push_back(page);
    }
    pages = std::move(sortedPages);
    placements = std::move(sortedPlacements);
}

std::string LineLabel(size_t lineNumber)
{
    return "Line " + std::to_string(lineNumber);
}

void Cook(std::istream& input, const std::filesystem::path& output, float boundRadius, bool spatialOrder)
{
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != kCsvHeader)
        throw std::runtime_error("The first CSV line must be " + std::string(kCsvHeader));
    std::vector<PlacementPage> pages;
    std::vector<PackedPlacement> placements;
    std::vector<Tree> page;
    page.reserve(PackedPlacementStore::kPlacementsPerPage);
    size_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        Tree tree{};
        int64 id = 0;
        if (!ParseTree(line, id, tree))
            throw std::runtime_error(LineLabel(lineNumber) + " is not a valid placement. Each row needs a page "
                                     "number from 0, finite coordinates within 1e12 meters, a yaw in degrees and a "
                                     "scale from 0.75 to 1.25.");
        if (!page.empty() && id == static_cast<int64>(pages.size()) + 1)
        {
            if (pages.size() + 1 == PackedPlacementStore::kMaxPages)
                throw std::runtime_error(LineLabel(lineNumber) + " starts page " + std::to_string(id) +
                                         ". An archive holds at most 1048576 pages. Split the forest into more "
                                         "archives.");
            AppendPage(page, boundRadius, pages, placements);
            page.clear();
        }
        if (id != static_cast<int64>(pages.size()))
            throw std::runtime_error(LineLabel(lineNumber) + " has page " + std::to_string(id) + ", but page " +
                                     std::to_string(pages.size()) + " or " + std::to_string(pages.size() + 1) +
                                     " comes next. Number the pages from 0 without gaps, in page order.");
        if (page.size() == PackedPlacementStore::kPlacementsPerPage)
            throw std::runtime_error(LineLabel(lineNumber) + " is tree 145 of page " + std::to_string(id) +
                                     ". A page holds at most 144 trees.");
        page.push_back(tree);
    }
    if (!input.eof())
        throw std::runtime_error("Could not read the placement CSV");
    if (page.empty())
        throw std::runtime_error("The placement CSV has no trees after its header");
    AppendPage(page, boundRadius, pages, placements);
    if (spatialOrder)
        OrderPagesSpatially(pages, placements);
    if (const std::string error = WritePackedPlacementStore(output, boundRadius, pages, placements); !error.empty())
        throw std::runtime_error(error);
    std::cout << "Wrote " << pages.size() << " pages and " << placements.size() << " placements\n";
}

constexpr std::string_view kUsage =
    "Usage: ForestPlacementCook placements.csv output.geforest boundRadiusMeters [--spatial-order]\n"
    "       ForestPlacementCook --overview input.geforest canopy.png output.glb minX minZ maxX maxZ "
    "canopyHeight tileSize";

void CookOverview(int argc, char** argv)
{
    ForestOverviewSource source{};
    if (argc != 11 || !ParseStrictDecimal(argv[5], source.MinX) || !ParseStrictDecimal(argv[6], source.MinZ) ||
        !ParseStrictDecimal(argv[7], source.MaxX) || !ParseStrictDecimal(argv[8], source.MaxZ) ||
        !ParseStrictDecimal(argv[9], source.Height) || source.MinX >= source.MaxX || source.MinZ >= source.MaxZ)
        throw std::runtime_error(std::string(kUsage));
    const auto size = ParseInt(argv[10]);
    const auto output = PathFromUtf8(argv[4]);
    if (!size || *size < 16 || *size > 64 || output.extension() != ".glb")
        throw std::runtime_error("Use a .glb output path and a tile size between 16 and 64 pixels");
    source.TileSize = static_cast<uint32>(*size);
    CookForestOverview(PathFromUtf8(argv[2]), PathFromUtf8(argv[3]), output, source);
    std::cout << "Wrote forest overview mesh and atlas\n";
}

void CookArchive(int argc, char** argv)
{
    float boundRadius = 0;
    const bool spatialOrder = argc == 5 && std::string_view(argv[4]) == "--spatial-order";
    if ((argc != 4 && !spatialOrder) || !ParseStrictDecimalFloat(argv[3], boundRadius) || boundRadius <= 0 ||
        boundRadius > std::numeric_limits<float>::max() / 2)
        throw std::runtime_error(std::string(kUsage));
    const auto source = PathFromUtf8(argv[1]);
    const auto output = PathFromUtf8(argv[2]);
    std::ifstream input(source);
    if (!input || output.extension() != ".geforest")
        throw std::runtime_error("A readable CSV and a .geforest output path are required");
    Cook(input, output, boundRadius, spatialOrder);
}

} // namespace
} // namespace GameEngine

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && std::string_view(argv[1]) == "--overview")
            GameEngine::CookOverview(argc, argv);
        else
            GameEngine::CookArchive(argc, argv);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "ForestPlacementCook: " << error.what() << '\n';
        return 1;
    }
}
