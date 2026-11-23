#include "ForestSplatBake.h"

#include "AssetCore/GUID.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/Forest/ForestSplatCells.h"
#include "FileSystem/FileSystem.h"
#include "Image/Decode.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Octahedral.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3F64.h"
#include "Types/Color.h"
#include "Types/ParseNumber.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using Mathematics::Vector3F64;
using Engine::Renderer::ForestSplatCell;
using Engine::Renderer::ForestSplatCells;

// Grid cells along the model height. With the leaf scale below, the splat crown covers what the impostor card covers.
// The cell size is the height divided by one less than this.
constexpr uint32 kHeightCells = 20;
// Scale of leaf surface area in the coverage: overlapping leaves inside a cell cover less than their sum.
constexpr double kLeafAreaCoverage = 0.12;
// The largest grid index on one axis: a cell stores each index in one byte.
constexpr uint32 kMaxCellsPerAxis = 255;
// Surface sample spacing, in cells.
constexpr double kSampleSpacingCells = 0.25;
// The height fraction below which the wind does not bend the tree.
constexpr float kRootHeightFraction = 0.04f;
// A cell with at least this leaf fraction byte is a leaf cell.
constexpr uint8 kLeafCellFraction = 128;
// A leaf cell whose normal has at least this |y| faces up or down. From the side it is seen edge-on.
constexpr float kFlatLeafNormalY = 0.65f;
// Coverage scale of a flat leaf cell, so that the crown fills from above.
constexpr uint32 kFlatLeafCoverageScale = 4;
// The largest value of a UNORM8 byte.
constexpr double kUnorm8Max = 255.0;
// Smallest length that a normal or direction may have before the bake divides by it.
constexpr double kMinLength = 1e-12;
// The largest texture or material file that the bake reads.
constexpr uint64 kMaxSourceBytes = 256ull << 20;
// Material document keys of the base color, the alpha cutoff and the albedo texture.
constexpr const char* kBaseColorKey = "baseColor";
constexpr const char* kAlphaCutoffKey = "alphaCutoff";
constexpr const char* kAlbedoMapKey = "albedoMap";

using Rgba = std::array<double, 4>;

// What the surface of one mesh contributes to the cells.
struct MeshSurface
{
    std::optional<Image::Image8> Albedo; // RGBA. No texture samples white.
    Rgba BaseColor{1.0, 1.0, 1.0, 1.0};
    double AlphaCutoff = 0.0; // Samples with less alpha are cut out.
    double LeafFraction = 0.0;
};

// The cell at grid (x, y, z) has its center at Origin + CellSize * (x, y, z).
struct SplatGrid
{
    Vector3F64 Origin;
    double CellSize = 0.0;
    std::array<uint32, 3> Shape{};
};

// The area-weighted sums of the surface samples in one cell.
struct CellSum
{
    double Area = 0.0;
    Vector3F64 Color;
    Vector3F64 Normal;
    double Leaf = 0.0;
};

uint8 Unorm8(double value)
{
    return static_cast<uint8>(std::nearbyint(std::clamp(value, 0.0, 1.0) * kUnorm8Max));
}

Image::Image8 DecodeAlbedo(const ByteBuffer& bytes, const std::string& name, const std::string& fix)
{
    auto image = Image::Decode8({bytes.Data(), bytes.Size()}, name, Image::DecodeChannels::Rgba);
    if (!image.Valid || image.Width == 0 || image.Height == 0)
        throw std::runtime_error("Could not decode the albedo texture " + name + ". " + fix);
    return image;
}

Image::Image8 ReadAlbedo(const std::filesystem::path& path, const std::string& fix)
{
    ByteBuffer bytes;
    if (!ReadFileBytesShared(path, bytes, kMaxSourceBytes) || bytes.Size() == 0)
        throw std::runtime_error("Could not read the albedo texture " + PathToUtf8(path) + " (at most 256 MiB). " +
                                 fix);
    return DecodeAlbedo(bytes, PathToUtf8(path), fix);
}

MeshSurface ProjectSurface(const std::filesystem::path& assetRoot, const ForestSplatMaterial& mesh)
{
    const std::string name = PathToUtf8(mesh.Material);
    MaterialAsset asset(GUID::Null(), assetRoot / mesh.Material);
    if (!asset.Load())
        throw std::runtime_error("Could not load material " + name + " for mesh " + std::to_string(mesh.Mesh) +
                                 ". Pass a .material path relative to the asset root.");
    const MaterialDocument& document = asset.GetDocument();
    MeshSurface surface;
    if (const auto color = document.properties.find(kBaseColorKey); color != document.properties.end())
        if (const auto* values = std::get_if<std::vector<float>>(&color->second))
            std::copy_n(values->begin(), std::min(values->size(), surface.BaseColor.size()), surface.BaseColor.begin());
    if (document.alphaMode == MaterialAlphaMode::Mask)
    {
        surface.LeafFraction = 1.0;
        surface.AlphaCutoff = kDefaultAlphaCutoff;
        if (const auto cutoff = document.properties.find(kAlphaCutoffKey); cutoff != document.properties.end())
            if (const auto* value = std::get_if<float>(&cutoff->second))
                surface.AlphaCutoff = *value;
    }
    if (!document.textures.contains(kAlbedoMapKey))
        return surface;
    const auto path = document.texturePaths.find(kAlbedoMapKey);
    if (path == document.texturePaths.end())
        throw std::runtime_error("Material " + name + " names its albedoMap by GUID only. Save the material in the "
                                 "editor so that the texture has its asset path.");
    surface.Albedo = ReadAlbedo(assetRoot / PathFromUtf8(path->second), "Fix the albedoMap path in " + name + ".");
    return surface;
}

MeshSurface ImportedSurface(const ModelAsset& model, uint32 meshIndex)
{
    const uint32 slot = model.GetMesh(meshIndex).MaterialIndex;
    const std::string fix = "Pass " + std::to_string(meshIndex) + "=<file.material> for that mesh.";
    if (slot >= model.GetMaterials().size())
        throw std::runtime_error("Mesh " + std::to_string(meshIndex) + " has no imported material. " + fix);
    const ImportedMaterialData& material = model.GetMaterials()[slot];
    MeshSurface surface;
    std::copy_n(material.DiffuseColor, surface.BaseColor.size(), surface.BaseColor.begin());
    if (material.AlphaMode == AlphaMode::Mask)
    {
        surface.LeafFraction = 1.0;
        surface.AlphaCutoff = material.AlphaCutoff;
    }
    const std::string& texture = material.DiffuseTexture;
    if (texture.empty())
        return surface;
    if (IsEmbeddedTextureRef(texture))
    {
        const auto index = ParseInt(std::string_view(texture).substr(kEmbeddedTexturePrefixLen));
        const auto& images = model.GetEmbeddedImages();
        if (!index || *index < 0 || static_cast<size_t>(*index) >= images.size() || images[*index].Data.Size() == 0)
            throw std::runtime_error("Mesh " + std::to_string(meshIndex) + " has an albedo texture that the model does not "
                                     "hold. " + fix);
        surface.Albedo = DecodeAlbedo(images[*index].Data, texture, fix);
        return surface;
    }
    surface.Albedo = ReadAlbedo(model.GetPath().parent_path() / PathFromUtf8(texture), fix);
    return surface;
}

// One surface per mesh of the model, in mesh order.
std::vector<MeshSurface> ResolveSurfaces(const std::filesystem::path& assetRoot, const ModelAsset& model,
                                         std::span<const ForestSplatMaterial> materials)
{
    const uint32 meshes = model.GetMeshCount();
    std::vector<std::optional<MeshSurface>> resolved(meshes);
    for (const ForestSplatMaterial& material : materials)
    {
        if (material.Mesh >= meshes)
            throw std::runtime_error("The model has no mesh " + std::to_string(material.Mesh) + ". Its meshes are 0 to " +
                                     std::to_string(meshes - 1) + ".");
        if (resolved[material.Mesh])
            throw std::runtime_error("Mesh " + std::to_string(material.Mesh) + " has two materials. Pass one.");
        resolved[material.Mesh] = ProjectSurface(assetRoot, material);
    }
    std::vector<MeshSurface> surfaces;
    surfaces.reserve(meshes);
    for (uint32 mesh = 0; mesh < meshes; ++mesh)
        surfaces.push_back(resolved[mesh] ? std::move(*resolved[mesh]) : ImportedSurface(model, mesh));
    return surfaces;
}

bool IsRestPose(const Mesh& mesh)
{
    constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return !mesh.IsSkinned() && mesh.ExtraPlacements.empty() &&
           std::equal(kIdentity.begin(), kIdentity.end(), mesh.SourceNodeTransform);
}

Vector3F64 Position(const Mesh& mesh, uint32 index)
{
    const float* position = mesh.Vertices[index].Position;
    return {position[0], position[1], position[2]};
}

SplatGrid MakeGrid(const ModelAsset& model)
{
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    Vector3F64 low(kInfinity, kInfinity, kInfinity);
    Vector3F64 high(-kInfinity, -kInfinity, -kInfinity);
    for (const Mesh& mesh : model.GetMeshes())
    {
        if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles)
            continue;
        if (!IsRestPose(mesh))
            throw std::runtime_error("Mesh " + mesh.Name + " is skinned, instanced or has a node transform. Export "
                                     "the tree as static meshes with the node transforms applied.");
        for (const uint32 index : mesh.Indices)
        {
            const Vector3F64 p = Position(mesh, index);
            low = {std::min(low.X, p.X), std::min(low.Y, p.Y), std::min(low.Z, p.Z)};
            high = {std::max(high.X, p.X), std::max(high.Y, p.Y), std::max(high.Z, p.Z)};
        }
    }
    const double height = high.Y - low.Y;
    if (!(height > 0.0))
        throw std::runtime_error("The tree model has no triangles of nonzero height.");
    SplatGrid grid{low, height / (kHeightCells - 1), {}};
    const Vector3F64 extent = (high - low) / grid.CellSize;
    const std::array<double, 3> cells{extent.X, extent.Y, extent.Z};
    for (size_t axis = 0; axis < cells.size(); ++axis)
    {
        const double last = std::floor(cells[axis] + 0.5);
        if (last > kMaxCellsPerAxis - 1)
            throw std::runtime_error("The tree model is more than 255 cells of height / 31 wide on one axis. Bake "
                                     "a tree that is at most about 8 times as wide as it is tall.");
        grid.Shape[axis] = static_cast<uint32>(last) + 1;
    }
    return grid;
}

// One barycentric weight triple per subtriangle centroid, when each edge splits into `divisions`.
std::vector<Vector3F64> BarycentricSamples(uint32 divisions)
{
    std::vector<Vector3F64> weights;
    weights.reserve(static_cast<size_t>(divisions) * divisions);
    const double n = divisions;
    for (uint32 row = 0; row < divisions; ++row)
        for (uint32 column = 0; column <= row; ++column)
        {
            const double u = (column + 1.0 / 3.0) / n;
            const double v = (n - row - 2.0 / 3.0) / n;
            weights.emplace_back(1.0 - (u + v), u, v);
        }
    for (uint32 row = 0; row + 1 < divisions; ++row)
        for (uint32 column = 0; column <= row; ++column)
        {
            const double u = (column + 2.0 / 3.0) / n;
            const double v = (n - row - 4.0 / 3.0) / n;
            weights.emplace_back(1.0 - (u + v), u, v);
        }
    return weights;
}

std::array<double, 256> MakeSrgbToLinearTable()
{
    std::array<double, 256> table{};
    for (size_t value = 0; value < table.size(); ++value)
    {
        const float c = static_cast<float>(value / kUnorm8Max);
        table[value] = ColorSRGB(c, c, c, 1.0f).ToLinear().r;
    }
    return table;
}

// Bilinear filter with repeat addressing, in linear color. The texture is sRGB with linear alpha.
Rgba SampleAlbedo(const Image::Image8& texture, double u, double v)
{
    static const std::array<double, 256> linear = MakeSrgbToLinearTable();
    const double px = u * texture.Width - 0.5;
    const double py = v * texture.Height - 0.5;
    const double fx = px - std::floor(px);
    const double fy = py - std::floor(py);
    const int64 width = texture.Width;
    const int64 height = texture.Height;
    const int64 x0 = (static_cast<int64>(std::floor(px)) % width + width) % width;
    const int64 y0 = (static_cast<int64>(std::floor(py)) % height + height) % height;
    const std::array<int64, 2> xs{x0, (x0 + 1) % width};
    const std::array<int64, 2> ys{y0, (y0 + 1) % height};
    const std::array<double, 4> weights{(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    Rgba result{};
    for (size_t tap = 0; tap < weights.size(); ++tap)
    {
        const size_t offset = static_cast<size_t>(ys[tap / 2] * width + xs[tap % 2]) * 4;
        for (size_t channel = 0; channel < 3; ++channel)
            result[channel] += linear[texture.Pixels[offset + channel]] * weights[tap];
        result[3] += texture.Pixels[offset + 3] / kUnorm8Max * weights[tap];
    }
    return result;
}

uint32 CellIndex(const SplatGrid& grid, const Vector3F64& position)
{
    const Vector3F64 cell = (position - grid.Origin) / grid.CellSize;
    const std::array<double, 3> axes{cell.X, cell.Y, cell.Z};
    std::array<uint32, 3> index{};
    for (size_t axis = 0; axis < axes.size(); ++axis)
        index[axis] = static_cast<uint32>(
            std::clamp(std::floor(axes[axis] + 0.5), 0.0, static_cast<double>(grid.Shape[axis] - 1)));
    return (index[0] * grid.Shape[1] + index[1]) * grid.Shape[2] + index[2];
}

void AccumulateTriangle(const Mesh& mesh, std::span<const uint32, 3> corners, const MeshSurface& surface,
                        const SplatGrid& grid, std::vector<CellSum>& sums)
{
    const std::array<Vector3F64, 3> p{Position(mesh, corners[0]), Position(mesh, corners[1]),
                                      Position(mesh, corners[2])};
    const double area = 0.5 * Vector3F64::Cross(p[1] - p[0], p[2] - p[0]).Length();
    if (!(area > 0.0))
        return;
    const double maxEdge = std::max({(p[0] - p[2]).Length(), (p[1] - p[0]).Length(), (p[2] - p[1]).Length()});
    const auto divisions =
        static_cast<uint32>(std::max(1.0, std::ceil(maxEdge / (kSampleSpacingCells * grid.CellSize))));
    const std::vector<Vector3F64> weights = BarycentricSamples(divisions);
    const double sampleArea = area / static_cast<double>(weights.size());
    const auto& v0 = mesh.Vertices[corners[0]];
    const auto& v1 = mesh.Vertices[corners[1]];
    const auto& v2 = mesh.Vertices[corners[2]];
    for (const Vector3F64& w : weights)
    {
        Rgba albedo{1.0, 1.0, 1.0, 1.0};
        if (surface.Albedo)
            albedo = SampleAlbedo(*surface.Albedo,
                                  w.X * v0.TexCoords[0] + w.Y * v1.TexCoords[0] + w.Z * v2.TexCoords[0],
                                  w.X * v0.TexCoords[1] + w.Y * v1.TexCoords[1] + w.Z * v2.TexCoords[1]);
        for (size_t channel = 0; channel < albedo.size(); ++channel)
            albedo[channel] *= surface.BaseColor[channel];
        if (albedo[3] < surface.AlphaCutoff)
            continue;
        const Vector3F64 position = p[0] * w.X + p[1] * w.Y + p[2] * w.Z;
        const Vector3F64 normal = Vector3F64(v0.Normal[0], v0.Normal[1], v0.Normal[2]) * w.X +
                                  Vector3F64(v1.Normal[0], v1.Normal[1], v1.Normal[2]) * w.Y +
                                  Vector3F64(v2.Normal[0], v2.Normal[1], v2.Normal[2]) * w.Z;
        CellSum& sum = sums[CellIndex(grid, position)];
        sum.Area += sampleArea;
        sum.Color = sum.Color + Vector3F64(albedo[0], albedo[1], albedo[2]) * sampleArea;
        sum.Normal = sum.Normal + normal / std::max(normal.Length(), kMinLength) * sampleArea;
        sum.Leaf += surface.LeafFraction * sampleArea;
    }
}

ForestSplatCell EncodeCell(const SplatGrid& grid, uint32 index, const CellSum& sum)
{
    const uint32 z = index % grid.Shape[2];
    const uint32 y = index / grid.Shape[2] % grid.Shape[1];
    const uint32 x = index / (grid.Shape[2] * grid.Shape[1]);
    // Projected area averaged over all view directions is half of the surface area.
    // Union coverage saturates as layers overlap inside a cell.
    const double coverageArea = sum.Area - sum.Leaf + sum.Leaf * kLeafAreaCoverage;
    uint32 coverage = std::max<uint8>(1, Unorm8(-std::expm1(-coverageArea / (2.0 * grid.CellSize * grid.CellSize))));
    const Vector3F64 color = sum.Color / sum.Area;
    const Vector3F64 normal = sum.Normal / sum.Area;
    const Mathematics::Vector2 octahedral = Mathematics::OctahedralEncode(
        static_cast<Mathematics::Vector3>(normal).NormalizeOr(Mathematics::Vector3(0.0f, 0.0f, 1.0f)));
    const uint8 normalX = Unorm8(octahedral.x);
    const uint8 normalY = Unorm8(octahedral.y);
    const uint8 bend = Unorm8(Math::SmoothStep(kRootHeightFraction, 1.0f,
                                                      static_cast<float>(y) / (kHeightCells - 1)));
    const uint8 leaf = Unorm8(sum.Leaf / sum.Area);
    const Mathematics::Vector3 decoded =
        Mathematics::OctahedralDecode(Mathematics::Vector2(static_cast<float>(normalX / kUnorm8Max), static_cast<float>(normalY / kUnorm8Max)));
    if (leaf >= kLeafCellFraction && std::abs(decoded.y) >= kFlatLeafNormalY)
        coverage = std::min<uint32>(UINT8_MAX, coverage * kFlatLeafCoverageScale);
    return {x | y << 8 | z << 16 | coverage << 24,
            Unorm8(color.X) | uint32{Unorm8(color.Y)} << 8 | uint32{Unorm8(color.Z)} << 16 | uint32{normalX} << 24,
            normalY | uint32{bend} << 8 | uint32{leaf} << 16};
}

void WriteSplats(const std::filesystem::path& output, const ForestSplatCells& cells)
{
    const std::vector<uint8> bytes = Engine::Renderer::EncodeForestSplatCells(cells);
    if (const auto parent = output.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
    const auto temp = FileSystem::MakeTemporarySiblingPath(output);
    std::ofstream file(temp, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
    {
        std::filesystem::remove(temp);
        throw std::runtime_error("Could not finish the splat bake");
    }
    if (!FileSystem::PublishFile(temp, output))
        throw std::runtime_error("Could not publish the splat bake");
}
} // namespace

uint32 CookForestSplats(const std::filesystem::path& assetRoot, const std::filesystem::path& model,
                        const std::filesystem::path& output, std::span<const ForestSplatMaterial> materials)
{
    ModelAsset tree(GUID::Null(), assetRoot / model);
    if (!tree.Load())
        throw std::runtime_error("Could not load the tree model " + PathToUtf8(model) +
                                 ". Pass a model path relative to the asset root.");
    const std::vector<MeshSurface> surfaces = ResolveSurfaces(assetRoot, tree, materials);
    const SplatGrid grid = MakeGrid(tree);
    std::vector<CellSum> sums(static_cast<size_t>(grid.Shape[0]) * grid.Shape[1] * grid.Shape[2]);
    for (uint32 meshIndex = 0; meshIndex < tree.GetMeshCount(); ++meshIndex)
    {
        const Mesh& mesh = tree.GetMesh(meshIndex);
        if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles)
            continue;
        for (size_t first = 0; first + 2 < mesh.Indices.size(); first += 3)
            AccumulateTriangle(mesh, std::span(mesh.Indices).subspan(first).first<3>(), surfaces[meshIndex], grid, sums);
    }

    ForestSplatCells cells{.Origin = static_cast<Mathematics::Vector3>(grid.Origin),
                           .CellSize = static_cast<float>(grid.CellSize)};
    for (uint32 index = 0; index < sums.size(); ++index)
    {
        if (sums[index].Area == 0.0)
            continue;
        if (cells.Cells.size() == ForestSplatCells::kMaxCells)
            throw std::runtime_error("The bake has more than 16384 cells. Bake a tree with less surface spread "
                                     "through its crown.");
        cells.Cells.push_back(EncodeCell(grid, index, sums[index]));
    }
    if (cells.Cells.empty())
        throw std::runtime_error("No surface samples survived the bake. Check the alpha cutoff and the albedo "
                                 "textures of the materials.");
    WriteSplats(output, cells);
    return static_cast<uint32>(cells.Cells.size());
}

} // namespace GameEngine
