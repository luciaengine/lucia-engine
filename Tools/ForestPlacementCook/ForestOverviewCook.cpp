#include "ForestOverviewCook.h"

#include "FileSystem/FileSystem.h"
#include "Image/Decode.h"
#include "Image/Encode.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace GameEngine;
using namespace GameEngine::PageStreaming;
using Rgba = std::array<uint8, 4>;
using Json = nlohmann::json;

// Keep page boundaries aligned through mip 3. Center at least four transparent texels on each side.
constexpr unsigned kMipAlignedTile = 8;
constexpr unsigned kMinGutter = kMipAlignedTile / 2;
constexpr unsigned kMaxAtlasSide = 8192;
constexpr unsigned kSamplesPerAxis = 2;
constexpr unsigned kSampleCount = kSamplesPerAxis * kSamplesPerAxis;

struct TopView
{
    Image::Image8 ImageData;
    ForestOverviewSource Source;
    double Radius;
    Rgba Transparent;
};

struct Tile
{
    double MinX;
    double MinZ;
    double MaxX;
    double MaxZ;
};

TopView ReadTopView(const std::filesystem::path& image, const ForestOverviewSource& source)
{
    std::ifstream file(image, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size > std::numeric_limits<int32>::max())
        throw std::runtime_error("Could not open the canopy source image");
    std::vector<uint8> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read the canopy source image");
    auto decoded = Image::Decode8(bytes, PathToUtf8(image), Image::DecodeChannels::Rgba);
    if (!decoded.Valid)
        throw std::runtime_error("Could not decode the canopy source image");
    std::array<uint64, 3> color{};
    uint64 weight = 0;
    for (size_t pixel = 0; pixel < decoded.Pixels.size(); pixel += 4)
    {
        const uint8 alpha = decoded.Pixels[pixel + 3];
        weight += alpha;
        for (size_t channel = 0; channel < color.size(); ++channel)
            color[channel] += static_cast<uint64>(decoded.Pixels[pixel + channel]) * alpha;
    }
    if (weight == 0)
        throw std::runtime_error("The canopy source image is fully transparent");
    const double radius = std::hypot(std::max(std::abs(source.MinX), std::abs(source.MaxX)),
                                     std::max(std::abs(source.MinZ), std::abs(source.MaxZ)));
    return {std::move(decoded), source, radius,
            {static_cast<uint8>(color[0] / weight), static_cast<uint8>(color[1] / weight),
             static_cast<uint8>(color[2] / weight), 0}};
}

void StampTree(std::span<Rgba> samples, const Tile& tile, const TopView& view, const PackedPlacement& tree)
{
    const auto& src = view.Source;
    const unsigned side = src.TileSize * kSamplesPerAxis;
    const double stepX = (tile.MaxX - tile.MinX) / side;
    const double stepZ = (tile.MaxZ - tile.MinZ) / side;
    if (!std::isnormal(stepX) || !std::isnormal(stepZ))
        throw std::runtime_error("The canopy projection has an unrepresentable scale");
    const double x = tree.LocalX();
    const double z = tree.LocalZ();
    const double scale = tree.UniformScale();
    const double radius = view.Radius * scale;
    const double yaw = tree.YawRadians();
    const double cosine = std::cos(yaw) / scale;
    const double sine = std::sin(yaw) / scale;
    const auto first = [side](double distance, double step)
    {
        return static_cast<unsigned>(std::clamp(std::ceil(distance / step - 0.5), 0.0, static_cast<double>(side)));
    };
    const unsigned xBegin = first(x - radius - tile.MinX, stepX);
    const unsigned xEnd = first(x + radius - tile.MinX, stepX);
    const unsigned zEnd = first(z + radius - tile.MinZ, stepZ);
    const auto& image = view.ImageData;
    for (unsigned row = first(z - radius - tile.MinZ, stepZ); row < zEnd; ++row)
    {
        const double dz = tile.MinZ + (row + 0.5) * stepZ - z;
        for (unsigned col = xBegin; col < xEnd; ++col)
        {
            const double dx = tile.MinX + (col + 0.5) * stepX - x;
            const double u = (dx * cosine - dz * sine - src.MinX) / (src.MaxX - src.MinX);
            const double v = (dx * sine + dz * cosine - src.MinZ) / (src.MaxZ - src.MinZ);
            if (u < 0.0 || v < 0.0 || u >= 1.0 || v >= 1.0)
                continue;
            const size_t imageRow = static_cast<unsigned>(v * image.Height);
            const size_t imageCol = static_cast<unsigned>(u * image.Width);
            const size_t offset = (imageRow * image.Width + imageCol) * 4;
            auto& sample = samples[row * side + col];
            // Keep the most opaque canopy hit. Placements from other pages never write this tile.
            if (image.Pixels[offset + 3] > sample[3])
                std::copy_n(image.Pixels.data() + offset, 4, sample.begin());
        }
    }
}

bool ResolveTile(std::span<uint8> atlas, unsigned width, unsigned x, unsigned z, const TopView& view,
                 std::span<const Rgba> samples)
{
    const unsigned side = view.Source.TileSize;
    const unsigned sampleSide = side * kSamplesPerAxis;
    bool visible = false;
    for (unsigned row = 0; row < side; ++row)
        for (unsigned col = 0; col < side; ++col)
        {
            std::array<unsigned, 3> color{};
            unsigned weight = 0;
            for (unsigned subZ = 0; subZ < kSamplesPerAxis; ++subZ)
                for (unsigned subX = 0; subX < kSamplesPerAxis; ++subX)
                {
                    const auto& pixel = samples[(row * kSamplesPerAxis + subZ) * sampleSide +
                                                col * kSamplesPerAxis + subX];
                    weight += pixel[3];
                    for (size_t channel = 0; channel < color.size(); ++channel)
                        color[channel] += pixel[channel] * pixel[3];
                }
            if (weight == 0)
                continue;
            const size_t offset = (static_cast<size_t>(z + row) * width + x + col) * 4;
            for (size_t channel = 0; channel < color.size(); ++channel)
                atlas[offset + channel] = static_cast<uint8>((color[channel] + weight / 2) / weight);
            atlas[offset + 3] = static_cast<uint8>((weight + kSampleCount / 2) / kSampleCount);
            visible |= atlas[offset + 3] >= 64;
        }
    return visible;
}

struct OverviewMesh
{
    std::vector<std::array<float, 3>> Positions;
    std::vector<std::array<float, 3>> Normals;
    std::vector<std::array<float, 2>> Uvs;
    std::vector<uint32> Indices;
};

void AppendQuad(OverviewMesh& mesh, const PlacementPage& page, const Tile& tile, double height,
                std::array<float, 2> lowUv, std::array<float, 2> highUv)
{
    // The default glTF import reflects X and reverses each triangle.
    const float lowX = -static_cast<float>(page.Origin[0] + tile.MinX);
    const float highX = -static_cast<float>(page.Origin[0] + tile.MaxX);
    const float lowZ = static_cast<float>(page.Origin[2] + tile.MinZ);
    const float highZ = static_cast<float>(page.Origin[2] + tile.MaxZ);
    const float y = static_cast<float>(page.Origin[1] + height);
    if (!std::isfinite(y) || !std::isfinite(lowX) || !std::isfinite(highX) || !std::isfinite(lowZ) ||
        !std::isfinite(highZ) || lowX <= highX || lowZ >= highZ)
        throw std::runtime_error("A page cannot be represented as a world-space overview quad");
    const auto first = static_cast<uint32>(mesh.Positions.size());
    mesh.Positions.insert(mesh.Positions.end(), {{lowX, y, lowZ}, {highX, y, lowZ}, {highX, y, highZ}, {lowX, y, highZ}});
    mesh.Normals.insert(mesh.Normals.end(), 4, {0, 1, 0});
    mesh.Uvs.insert(mesh.Uvs.end(), {lowUv, {highUv[0], lowUv[1]}, highUv, {lowUv[0], highUv[1]}});
    mesh.Indices.insert(mesh.Indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

void WriteGlb(const std::filesystem::path& output, const OverviewMesh& mesh, const std::vector<uint8>& png)
{
    static_assert(std::endian::native == std::endian::little);
    const std::array data{std::as_bytes(std::span(mesh.Positions)), std::as_bytes(std::span(mesh.Normals)),
                          std::as_bytes(std::span(mesh.Uvs)), std::as_bytes(std::span(mesh.Indices)),
                          std::as_bytes(std::span(png))};
    constexpr std::array targets{34962, 34962, 34962, 34963};
    constexpr std::array formats{"VEC3", "VEC3", "VEC2", "SCALAR"};
    Json views = Json::array();
    Json accessors = Json::array();
    uint32 offset = 0;
    for (size_t i = 0; i < data.size(); ++i)
    {
        Json view{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", data[i].size()}};
        if (i < targets.size())
        {
            view["target"] = targets[i];
            accessors.push_back({{"bufferView", i}, {"componentType", i == 3 ? 5125 : 5126},
                                 {"count", i == 3 ? mesh.Indices.size() : mesh.Positions.size()}, {"type", formats[i]}});
        }
        views.push_back(std::move(view));
        offset += static_cast<uint32>(data[i].size());
    }
    auto min = mesh.Positions.front();
    auto max = min;
    for (const auto& position : mesh.Positions)
        for (size_t axis = 0; axis < position.size(); ++axis)
        {
            min[axis] = std::min(min[axis], position[axis]);
            max[axis] = std::max(max[axis], position[axis]);
        }
    accessors[0]["min"] = min;
    accessors[0]["max"] = max;
    Json primitive{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                   {"indices", 3}, {"material", 0}};
    Json maskedMaterial{{"name", "Forest Canopy"}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.25},
                        {"doubleSided", true}, {"extras", {{"luciaForestCanopyCoverage", true}}},
                        {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}},
                                                  {"metallicFactor", 0}, {"roughnessFactor", 1}}}};
    Json asset{{"asset", {{"version", "2.0"}}}, {"scene", 0},
               {"scenes", Json::array({{{"nodes", {0}}}})}, {"nodes", Json::array({{{"mesh", 0}}})},
               {"meshes", Json::array({{{"primitives", Json::array({primitive})}}})},
               {"buffers", Json::array({{{"byteLength", offset}}})}, {"bufferViews", views}, {"accessors", accessors},
               {"images", Json::array({{{"bufferView", 4}, {"mimeType", "image/png"}}})},
               {"textures", Json::array({{{"source", 0}}})}, {"materials", Json::array({maskedMaterial})}};
    std::string metadata = asset.dump();
    metadata.append((4 - metadata.size() % 4) % 4, ' ');
    const uint32 padding = (4 - offset % 4) % 4;
    const uint32 jsonBytes = static_cast<uint32>(metadata.size());
    const std::array<uint32, 5> header{0x46546C67, 2, 28 + jsonBytes + offset + padding, jsonBytes, 0x4E4F534A};
    const std::array<uint32, 2> binHeader{offset + padding, 0x004E4942};
    if (const auto parent = output.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
    const auto temp = FileSystem::MakeTemporarySiblingPath(output);
    std::ofstream file(temp, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    file.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
    file.write(reinterpret_cast<const char*>(binHeader.data()), sizeof(binHeader));
    for (const auto bytes : data)
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.write("\0\0\0", padding);
    file.close();
    if (!file)
    {
        std::filesystem::remove(temp);
        throw std::runtime_error("Could not finish the overview GLB");
    }
    if (!FileSystem::PublishFile(temp, output))
        throw std::runtime_error("Could not publish the overview GLB");
}
} // namespace

void CookForestOverview(const std::filesystem::path& archive, const std::filesystem::path& image,
                        const std::filesystem::path& output, const ForestOverviewSource& source)
{
    PackedPlacementStore store;
    if (const std::string error = store.Open(archive); !error.empty())
        throw std::runtime_error(error);
    const unsigned cell = (source.TileSize + 2 * kMinGutter + kMipAlignedTile - 1) &
                          ~(kMipAlignedTile - 1);
    const unsigned gutter = (cell - source.TileSize) / 2;
    const unsigned columns = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(store.Pages().size()))));
    const unsigned rows = (static_cast<unsigned>(store.Pages().size()) + columns - 1) / columns;
    const unsigned width = columns * cell;
    const unsigned height = rows * cell;
    if (width > kMaxAtlasSide || height > kMaxAtlasSide)
        throw std::runtime_error("Overview atlas exceeds 8192 pixels per side. Reduce tile size or split the forest.");

    const auto view = ReadTopView(image, source);
    std::vector<uint8> atlas(static_cast<size_t>(width) * height * 4);
    for (size_t offset = 0; offset < atlas.size(); offset += 4)
        std::copy(view.Transparent.begin(), view.Transparent.end(), atlas.data() + offset);
    std::vector<Rgba> samples(static_cast<size_t>(source.TileSize) * source.TileSize * kSampleCount);
    OverviewMesh mesh;
    std::ifstream file(archive, std::ios::binary);
    std::array<PackedPlacement, PackedPlacementStore::kPlacementsPerPage> packed;
    for (uint32 index = 0; index < store.Pages().size(); ++index)
    {
        const auto& page = store.Pages()[index];
        const auto range = *store.ReadRange(index);
        const auto trees = std::span(packed).first(page.Count);
        file.seekg(static_cast<std::streamoff>(range.Offset));
        if (!file.read(reinterpret_cast<char*>(trees.data()), static_cast<std::streamsize>(range.Bytes)) ||
            !store.VerifyPage(index, {reinterpret_cast<const uint8*>(trees.data()), trees.size_bytes()}))
            throw std::runtime_error("Could not verify placement page " + std::to_string(index));

        Tile tile{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        double scaleSum = 0;
        for (const auto& tree : trees)
        {
            const double scale = tree.UniformScale();
            const double radius = view.Radius * scale;
            scaleSum += scale;
            const double x = tree.LocalX();
            const double z = tree.LocalZ();
            tile.MinX = std::min(tile.MinX, x - radius);
            tile.MinZ = std::min(tile.MinZ, z - radius);
            tile.MaxX = std::max(tile.MaxX, x + radius);
            tile.MaxZ = std::max(tile.MaxZ, z + radius);
        }
        const double canopyHeight = source.Height * scaleSum / page.Count;
        if (tile.MinX < page.LocalMin[0] || tile.MinZ < page.LocalMin[2] ||
            tile.MaxX > page.LocalMax[0] || tile.MaxZ > page.LocalMax[2] ||
            canopyHeight < page.LocalMin[1] || canopyHeight > page.LocalMax[1])
            throw std::runtime_error("The canopy projection exceeds the placement bounds on page " + std::to_string(index));

        const unsigned x = index % columns * cell + gutter;
        const unsigned z = index / columns * cell + gutter;
        std::fill(samples.begin(), samples.end(), Rgba{});
        for (const auto& tree : trees)
            StampTree(samples, tile, view, tree);
        if (!ResolveTile(atlas, width, x, z, view, samples))
            throw std::runtime_error("No visible canopy in page " + std::to_string(index) + ". Increase the tile size.");
        AppendQuad(mesh, page, tile, canopyHeight, {float(x) / width, float(z) / height},
                   {float(x + source.TileSize) / width, float(z + source.TileSize) / height});
    }
    const auto png = Image::EncodePng(atlas, width, height);
    if (png.empty())
        throw std::runtime_error("Could not encode the overview texture");
    WriteGlb(output, mesh, png);
}
