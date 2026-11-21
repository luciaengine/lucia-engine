#include "ForestOverviewCook.h"

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/ParseNumber.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace GameEngine;
using namespace GameEngine::PageStreaming;

struct Tree
{
    double X;
    double Y;
    double Z;
    double Yaw;
    double Scale;
};

constexpr double kMinScale = 0.75;
constexpr double kMaxScale = 1.25;
constexpr double kCentimetersPerMeter = 100.0;
constexpr uint16 kLargestWord = std::numeric_limits<uint16>::max();

bool ParseTree(std::string_view line, int64& page, Tree& tree)
{
    std::array<std::string_view, 6> fields{};
    for (size_t field = 0; field + 1 < fields.size(); ++field)
    {
        const size_t comma = line.find(',');
        if (comma == std::string_view::npos)
            return false;
        fields[field] = line.substr(0, comma);
        line.remove_prefix(comma + 1);
    }
    fields.back() = line;
    const auto pageId = ParseInt(fields.front());
    if (!pageId || *pageId < 0)
        return false;
    page = *pageId;
    return ParseStrictDecimal(fields[1], tree.X) && ParseStrictDecimal(fields[2], tree.Y) &&
           ParseStrictDecimal(fields[3], tree.Z) && ParseStrictDecimal(fields[4], tree.Yaw) &&
           ParseStrictDecimal(fields[5], tree.Scale) && tree.Scale >= kMinScale && tree.Scale <= kMaxScale;
}

void AppendPage(std::span<const Tree> trees, float rootRadius, std::vector<PlacementPage>& pages,
                std::vector<PackedPlacement>& placements)
{
    const std::string label = "Page " + std::to_string(pages.size());
    if (placements.size() > std::numeric_limits<uint32>::max() - trees.size())
        throw std::runtime_error("Placement count exceeds the archive limit");
    double x = trees.front().X;
    const double y = trees.front().Y;
    double z = trees.front().Z;
    for (const Tree& tree : trees)
    {
        if (tree.Y != y)
            throw std::runtime_error(label + " requires one common root elevation");
        x = std::min(x, tree.X);
        z = std::min(z, tree.Z);
    }

    PlacementPage page{{x, y, z}, {}, {}, 0, static_cast<uint32>(placements.size()), static_cast<uint32>(trees.size())};
    // Use the largest representable scale so rounding and shader decoding cannot shrink the bounds.
    const float radius = std::nextafter(rootRadius * static_cast<float>(kMaxScale), std::numeric_limits<float>::infinity());
    page.LocalMin = {-radius, -radius, -radius};
    page.LocalMax = {radius, radius, radius};
    for (const Tree& tree : trees)
    {
        const double localX = (tree.X - x) * kCentimetersPerMeter;
        const double localZ = (tree.Z - z) * kCentimetersPerMeter;
        if (localX >= kLargestWord + 0.5 || localZ >= kLargestWord + 0.5)
            throw std::runtime_error(label + " exceeds the 655.35 meter horizontal packing range");
        const auto encodedX = static_cast<uint16>(std::lround(localX));
        const auto encodedZ = static_cast<uint16>(std::lround(localZ));
        const uint16 yaw = static_cast<uint16>(std::lround(std::fmod(tree.Yaw, 360.0) * (65536.0 / 360.0)));
        const auto scale = static_cast<uint16>(std::lround((tree.Scale - kMinScale) *
                                                          (kLargestWord / (kMaxScale - kMinScale))));
        placements.push_back({encodedX, encodedZ, yaw, scale});
        page.LocalMax[0] = std::max(page.LocalMax[0], static_cast<float>(localX / kCentimetersPerMeter) + radius);
        page.LocalMax[2] = std::max(page.LocalMax[2], static_cast<float>(localZ / kCentimetersPerMeter) + radius);
    }
    // A packed position may round up by half a centimeter.
    page.LocalMax[0] = std::nextafter(page.LocalMax[0] + 0.005f, std::numeric_limits<float>::infinity());
    page.LocalMax[2] = std::nextafter(page.LocalMax[2] + 0.005f, std::numeric_limits<float>::infinity());
    pages.push_back(page);
}

uint32 QuantizePageCenter(double center, double min, double max)
{
    if (min == max)
        return 0;
    // Halve first when the full interval is wider than a double can represent.
    if (!std::isfinite(max - min))
    {
        center *= 0.5;
        min *= 0.5;
        max *= 0.5;
    }
    return static_cast<uint32>(std::clamp((center - min) / (max - min), 0.0, 1.0) * 65535.0);
}

void OrderPagesSpatially(std::vector<PlacementPage>& pages, std::vector<PackedPlacement>& placements)
{
    if (pages.size() < 2)
        return;
    std::vector<std::array<double, 2>> centers;
    centers.reserve(pages.size());
    for (const auto& page : pages)
    {
        // Bounds and their centers come from the same extents used by stand culling.
        const double x = std::midpoint(page.Origin[0] + page.LocalMin[0], page.Origin[0] + page.LocalMax[0]);
        const double z = std::midpoint(page.Origin[2] + page.LocalMin[2], page.Origin[2] + page.LocalMax[2]);
        if (!std::isfinite(x) || !std::isfinite(z))
            throw std::runtime_error("Page center is outside the supported coordinate range");
        centers.push_back({x, z});
    }
    auto min = centers.front();
    auto max = min;
    for (const auto& center : centers)
        for (size_t axis = 0; axis < center.size(); ++axis)
        {
            min[axis] = std::min(min[axis], center[axis]);
            max[axis] = std::max(max[axis], center[axis]);
        }

    std::vector<std::pair<uint64, size_t>> order;
    order.reserve(pages.size());
    for (size_t page = 0; page < pages.size(); ++page)
    {
        const uint32 x = QuantizePageCenter(centers[page][0], min[0], max[0]);
        const uint32 z = QuantizePageCenter(centers[page][1], min[1], max[1]);
        order.emplace_back(PageMortonCode(x, z), page);
    }
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

void Cook(std::istream& input, const std::filesystem::path& output, float radius, bool spatialOrder)
{
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != "page,x,y,z,yawDegrees,scale")
        throw std::runtime_error("CSV header must be page,x,y,z,yawDegrees,scale");
    std::vector<PlacementPage> pages;
    std::vector<PackedPlacement> placements;
    std::vector<Tree> page;
    page.reserve(PackedPlacementStore::kPlacementsPerPage);
    size_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        Tree tree{};
        int64 id = 0;
        if (!ParseTree(line, id, tree))
            throw std::runtime_error("Invalid placement on line " + std::to_string(lineNumber));
        if (!page.empty() && id == static_cast<int64>(pages.size()) + 1)
        {
            AppendPage(page, radius, pages, placements);
            page.clear();
        }
        if (id != static_cast<int64>(pages.size()) || page.size() == PackedPlacementStore::kPlacementsPerPage)
            throw std::runtime_error("Page IDs must be contiguous in stand mesh order, with at most 144 trees per page");
        page.push_back(tree);
    }
    if (!input.eof())
        throw std::runtime_error("Could not read placement CSV");
    if (!page.empty())
        AppendPage(page, radius, pages, placements);
    if (spatialOrder)
        OrderPagesSpatially(pages, placements);
    if (const std::string error = WritePackedPlacementStore(output, pages, placements); !error.empty())
        throw std::runtime_error(error);
    std::cout << "Wrote " << pages.size() << " pages and " << placements.size() << " placements\n";
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && std::string_view(argv[1]) == "--overview")
        {
            ForestOverviewSource source{};
            if (argc != 11 || !GameEngine::ParseStrictDecimal(argv[5], source.MinX) ||
                !GameEngine::ParseStrictDecimal(argv[6], source.MinZ) ||
                !GameEngine::ParseStrictDecimal(argv[7], source.MaxX) ||
                !GameEngine::ParseStrictDecimal(argv[8], source.MaxZ) ||
                !GameEngine::ParseStrictDecimal(argv[9], source.Height) ||
                source.MinX >= source.MaxX || source.MinZ >= source.MaxZ)
                throw std::runtime_error("Usage: ForestPlacementCook --overview input.geforest canopy.png output.glb "
                                         "minX minZ maxX maxZ canopyHeight tileSize");
            const auto size = GameEngine::ParseInt(argv[10]);
            const auto output = GameEngine::PathFromUtf8(argv[4]);
            if (!size || *size < 16 || *size > 64 || output.extension() != ".glb")
                throw std::runtime_error("Use a .glb output path and a tile size between 16 and 64 pixels");
            source.TileSize = static_cast<unsigned>(*size);
            CookForestOverview(GameEngine::PathFromUtf8(argv[2]), GameEngine::PathFromUtf8(argv[3]), output, source);
            std::cout << "Wrote forest overview mesh and atlas\n";
            return 0;
        }
        float radius = 0;
        const bool spatialOrder = argc == 5 && std::string_view(argv[4]) == "--spatial-order";
        if ((argc != 4 && !spatialOrder) || !GameEngine::ParseStrictDecimalFloat(argv[3], radius) || radius <= 0 ||
            radius > std::numeric_limits<float>::max() / 2)
            throw std::runtime_error("Usage: ForestPlacementCook placements.csv output.geforest rootRadiusMeters [--spatial-order]");
        const auto source = GameEngine::PathFromUtf8(argv[1]);
        const auto output = GameEngine::PathFromUtf8(argv[2]);
        std::ifstream input(source);
        if (!input || output.extension() != ".geforest")
            throw std::runtime_error("A readable CSV and a .geforest output path are required");
        Cook(input, output, radius, spatialOrder);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "ForestPlacementCook: " << error.what() << '\n';
        return 1;
    }
}
