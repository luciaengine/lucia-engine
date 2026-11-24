#include "ForestOverviewCook.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "Image/Decode.h"
#include "Image/Encode.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using namespace GameEngine::PageStreaming;
using Rgba = std::array<uint8, 4>;
using Json = nlohmann::json;

// Keep page boundaries aligned through mip 3. Center at least four transparent texels on each side.
constexpr uint32 kMipAlignedTile = 8;
constexpr uint32 kMinGutter = kMipAlignedTile / 2;
constexpr uint32 kMaxAtlasSide = 8192;
constexpr uint32 kSamplesPerAxis = 2;
constexpr uint32 kSampleCount = kSamplesPerAxis * kSamplesPerAxis;
constexpr uint64 kMaxImageBytes = 256ull << 20;

// glTF 2.0 buffer view targets, accessor component types and GLB container words.
constexpr int kGltfArrayBuffer = 34962;
constexpr int kGltfElementArrayBuffer = 34963;
constexpr int kGltfFloat = 5126;
constexpr int kGltfUnsignedInt = 5125;
constexpr uint32 kGlbMagic = 0x46546C67;
constexpr uint32 kGlbVersion = 2;
constexpr uint32 kGlbJsonChunk = 0x4E4F534A;
constexpr uint32 kGlbBinaryChunk = 0x004E4942;
// The 12-byte file header and the two 8-byte chunk headers.
constexpr uint32 kGlbHeaderAndChunkBytes = 28;

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
    ByteBuffer bytes;
    if (!ReadFileBytesShared(image, bytes, kMaxImageBytes) || bytes.Size() == 0)
        throw std::runtime_error("Could not read the canopy source image (at most 256 MiB)");
    auto decoded = Image::Decode8({bytes.Data(), bytes.Size()}, PathToUtf8(image), Image::DecodeChannels::Rgba);
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

// The first sample whose center is at or past distance, clamped to [0, side].
uint32 FirstSampleAt(double distance, double step, uint32 side)
{
    return static_cast<uint32>(std::clamp(std::ceil(distance / step - 0.5), 0.0, static_cast<double>(side)));
}

void StampTree(std::span<Rgba> samples, const Tile& tile, const TopView& view, const PackedPlacement& tree)
{
    const auto& src = view.Source;
    const uint32 side = src.TileSize * kSamplesPerAxis;
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
    const uint32 xBegin = FirstSampleAt(x - radius - tile.MinX, stepX, side);
    const uint32 xEnd = FirstSampleAt(x + radius - tile.MinX, stepX, side);
    const uint32 zEnd = FirstSampleAt(z + radius - tile.MinZ, stepZ, side);
    const auto& image = view.ImageData;
    for (uint32 row = FirstSampleAt(z - radius - tile.MinZ, stepZ, side); row < zEnd; ++row)
    {
        const double dz = tile.MinZ + (row + 0.5) * stepZ - z;
        for (uint32 col = xBegin; col < xEnd; ++col)
        {
            const double dx = tile.MinX + (col + 0.5) * stepX - x;
            const double u = (dx * cosine - dz * sine - src.MinX) / (src.MaxX - src.MinX);
            const double v = (dx * sine + dz * cosine - src.MinZ) / (src.MaxZ - src.MinZ);
            if (u < 0.0 || v < 0.0 || u >= 1.0 || v >= 1.0)
                continue;
            const size_t imageRow = static_cast<uint32>(v * image.Height);
            const size_t imageCol = static_cast<uint32>(u * image.Width);
            const size_t offset = (imageRow * image.Width + imageCol) * 4;
            auto& sample = samples[row * side + col];
            // Keep the most opaque canopy hit. Placements from other pages never write this tile.
            if (image.Pixels[offset + 3] > sample[3])
                std::copy_n(image.Pixels.data() + offset, 4, sample.begin());
        }
    }
}

bool ResolveTile(std::span<uint8> atlas, uint32 width, uint32 x, uint32 z, const TopView& view,
                 std::span<const Rgba> samples)
{
    const uint32 side = view.Source.TileSize;
    const uint32 sampleSide = side * kSamplesPerAxis;
    bool visible = false;
    for (uint32 row = 0; row < side; ++row)
        for (uint32 col = 0; col < side; ++col)
        {
            std::array<uint32, 3> color{};
            uint32 weight = 0;
            for (uint32 subZ = 0; subZ < kSamplesPerAxis; ++subZ)
                for (uint32 subX = 0; subX < kSamplesPerAxis; ++subX)
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
    std::vector<Mathematics::Vector3> Positions;
    std::vector<Mathematics::Vector3> Normals;
    std::vector<Mathematics::Vector2> Uvs;
    std::vector<uint32> Indices;
};

void AppendQuad(OverviewMesh& mesh, const PlacementPage& page, const Tile& tile, double height,
                Mathematics::Vector2 lowUv, Mathematics::Vector2 highUv)
{
    // The default glTF import reflects X and reverses each triangle.
    const auto sectorOrigin = Mathematics::WorldPosition::FromSector(page.OverviewSector(), {});
    const auto local = page.Origin - sectorOrigin;
    const float lowX = -static_cast<float>(local.x + tile.MinX);
    const float highX = -static_cast<float>(local.x + tile.MaxX);
    const float lowZ = static_cast<float>(local.z + tile.MinZ);
    const float highZ = static_cast<float>(local.z + tile.MaxZ);
    const float y = static_cast<float>(local.y + height);
    if (!std::isfinite(y) || !std::isfinite(lowX) || !std::isfinite(highX) || !std::isfinite(lowZ) ||
        !std::isfinite(highZ) || lowX <= highX || lowZ >= highZ)
        throw std::runtime_error("A page cannot be represented as a sector-local overview quad");
    const auto first = static_cast<uint32>(mesh.Positions.size());
    mesh.Positions.insert(mesh.Positions.end(), {Mathematics::Vector3(lowX, y, lowZ), Mathematics::Vector3(highX, y, lowZ),
                                                 Mathematics::Vector3(highX, y, highZ), Mathematics::Vector3(lowX, y, highZ)});
    mesh.Normals.insert(mesh.Normals.end(), 4, Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    mesh.Uvs.insert(mesh.Uvs.end(), {lowUv, Mathematics::Vector2(highUv.x, lowUv.y), highUv,
                                     Mathematics::Vector2(lowUv.x, highUv.y)});
    mesh.Indices.insert(mesh.Indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

void WriteGlb(const std::filesystem::path& output, const OverviewMesh& mesh, const std::vector<uint8>& png)
{
    static_assert(std::endian::native == std::endian::little);
    const std::array data{std::as_bytes(std::span(mesh.Positions)), std::as_bytes(std::span(mesh.Normals)),
                          std::as_bytes(std::span(mesh.Uvs)), std::as_bytes(std::span(mesh.Indices)),
                          std::as_bytes(std::span(png))};
    constexpr std::array targets{kGltfArrayBuffer, kGltfArrayBuffer, kGltfArrayBuffer, kGltfElementArrayBuffer};
    constexpr std::array formats{"VEC3", "VEC3", "VEC2", "SCALAR"};
    constexpr size_t kIndexView = 3;
    Json views = Json::array();
    Json accessors = Json::array();
    uint32 offset = 0;
    for (size_t viewIndex = 0; viewIndex < data.size(); ++viewIndex)
    {
        Json view{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", data[viewIndex].size()}};
        if (viewIndex < targets.size())
        {
            const bool indices = viewIndex == kIndexView;
            view["target"] = targets[viewIndex];
            accessors.push_back({{"bufferView", viewIndex},
                                 {"componentType", indices ? kGltfUnsignedInt : kGltfFloat},
                                 {"count", indices ? mesh.Indices.size() : mesh.Positions.size()},
                                 {"type", formats[viewIndex]}});
        }
        views.push_back(std::move(view));
        offset += static_cast<uint32>(data[viewIndex].size());
    }
    Mathematics::Vector3 min = mesh.Positions.front();
    Mathematics::Vector3 max = min;
    for (const Mathematics::Vector3& position : mesh.Positions)
        for (size_t axis = 0; axis < 3; ++axis)
        {
            min[axis] = std::min(min[axis], position[axis]);
            max[axis] = std::max(max[axis], position[axis]);
        }
    accessors[0]["min"] = {min.x, min.y, min.z};
    accessors[0]["max"] = {max.x, max.y, max.z};
    Json primitive{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                   {"indices", 3}, {"material", 0}};
    Json maskedMaterial{{"name", "Forest Canopy"}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.25},
                        {"doubleSided", true}, {"extras", {{"luciaForestCanopyCoverage", true}}},
                        {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}},
                                                  {"metallicFactor", 0}, {"roughnessFactor", 1}}}};
    Json asset{{"asset", {{"version", "2.0"}}}, {"scene", 0},
               {"scenes", Json::array({{{"nodes", {0}}}})}, {"nodes", Json::array({{{"mesh", 0}}})},
               {"meshes", Json::array({{{"primitives", Json::array({primitive})},
                                        {"extras", {{"luciaForestSectorLocal", true}}}}})},
               {"buffers", Json::array({{{"byteLength", offset}}})}, {"bufferViews", views}, {"accessors", accessors},
               {"images", Json::array({{{"bufferView", 4}, {"mimeType", "image/png"}}})},
               {"textures", Json::array({{{"source", 0}}})}, {"materials", Json::array({maskedMaterial})}};
    std::string metadata = asset.dump();
    metadata.append((4 - metadata.size() % 4) % 4, ' ');
    const uint32 padding = (4 - offset % 4) % 4;
    const uint32 jsonBytes = static_cast<uint32>(metadata.size());
    const std::array<uint32, 5> header{kGlbMagic, kGlbVersion, kGlbHeaderAndChunkBytes + jsonBytes + offset + padding,
                                       jsonBytes, kGlbJsonChunk};
    const std::array<uint32, 2> binHeader{offset + padding, kGlbBinaryChunk};
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
    const uint32 cell = (source.TileSize + 2 * kMinGutter + kMipAlignedTile - 1) &
                          ~(kMipAlignedTile - 1);
    const uint32 gutter = (cell - source.TileSize) / 2;
    const uint32 columns = static_cast<uint32>(std::ceil(std::sqrt(static_cast<double>(store.Pages().size()))));
    const uint32 rows = (static_cast<uint32>(store.Pages().size()) + columns - 1) / columns;
    const uint32 width = columns * cell;
    const uint32 height = rows * cell;
    if (width > kMaxAtlasSide || height > kMaxAtlasSide)
        throw std::runtime_error("Overview atlas exceeds 8192 pixels per side. Reduce tile size or split the forest.");

    const auto view = ReadTopView(image, source);
    std::vector<uint8> atlas(static_cast<size_t>(width) * height * 4);
    for (size_t offset = 0; offset < atlas.size(); offset += 4)
        std::copy(view.Transparent.begin(), view.Transparent.end(), atlas.data() + offset);
    std::vector<Rgba> samples(static_cast<size_t>(source.TileSize) * source.TileSize * kSampleCount);
    OverviewMesh mesh;
    SharedFileReader file;
    if (!file.Open(archive))
        throw std::runtime_error("Could not open the placement archive");
    std::array<PackedPlacement, PackedPlacementStore::kPlacementsPerPage> packed;
    for (uint32 index = 0; index < store.Pages().size(); ++index)
    {
        const auto& page = store.Pages()[index];
        const auto range = *store.ReadRange(index);
        const auto trees = std::span(packed).first(page.Count);
        if (!file.SeekTo(range.Offset) ||
            file.Read(trees.data(), range.Bytes) != static_cast<int64>(range.Bytes) ||
            !store.VerifyPage(index, {reinterpret_cast<const uint8*>(trees.data()), trees.size_bytes()}))
            throw std::runtime_error("Could not verify placement page " + std::to_string(index));

        Tile tile{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        double heightSum = 0;
        for (const auto& tree : trees)
        {
            const double scale = tree.UniformScale();
            const double radius = view.Radius * scale;
            heightSum += tree.LocalY() + source.Height * scale;
            const double x = tree.LocalX();
            const double z = tree.LocalZ();
            tile.MinX = std::min(tile.MinX, x - radius);
            tile.MinZ = std::min(tile.MinZ, z - radius);
            tile.MaxX = std::max(tile.MaxX, x + radius);
            tile.MaxZ = std::max(tile.MaxZ, z + radius);
        }
        const double canopyHeight = heightSum / page.Count;
        if (tile.MinX < page.LocalMin.x || tile.MinZ < page.LocalMin.z ||
            tile.MaxX > page.LocalMax.x || tile.MaxZ > page.LocalMax.z ||
            canopyHeight < page.LocalMin.y || canopyHeight > page.LocalMax.y)
            throw std::runtime_error("The canopy projection exceeds the placement bounds on page " + std::to_string(index) +
                                     ". Use canopy bounds and a height inside the bound radius the archive was cooked with, or cook it with a larger radius.");

        const uint32 x = index % columns * cell + gutter;
        const uint32 z = index / columns * cell + gutter;
        std::fill(samples.begin(), samples.end(), Rgba{});
        for (const auto& tree : trees)
            StampTree(samples, tile, view, tree);
        if (!ResolveTile(atlas, width, x, z, view, samples))
            throw std::runtime_error("No visible canopy in page " + std::to_string(index) + ". Increase the tile size.");
        AppendQuad(mesh, page, tile, canopyHeight, Mathematics::Vector2(float(x) / width, float(z) / height),
                   Mathematics::Vector2(float(x + source.TileSize) / width, float(z + source.TileSize) / height));
    }
    const auto png = Image::EncodePng(atlas, width, height);
    if (png.empty())
        throw std::runtime_error("Could not encode the overview texture");
    WriteGlb(output, mesh, png);
}
} // namespace GameEngine
