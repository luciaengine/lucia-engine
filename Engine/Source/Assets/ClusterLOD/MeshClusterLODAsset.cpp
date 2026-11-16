#include "Assets/ClusterLOD/MeshClusterLODAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/ImportedMaterialCache.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/ScopedFileLock.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "Types/FlatSet.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <type_traits>

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{

constexpr uint64 kMagic = 0x315453554c434547ull;
constexpr uint32 kVersion = 4;
// The newest meshopt vertex codec version and its default compression level. The index codec
// uses the library default, version 1, the newest.
constexpr int kPageVertexVersion = 1;
constexpr int kPageVertexLevel = 2;
constexpr uint64 kMaximumFileBytes = MeshClusterLODManifest::kMaximumFileBytes;
constexpr uint64 kMaximumImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint32 kMaximumMaterialBytes = 1024u * 1024u;
// Meshes and placements of one model.
constexpr uint32 kMaximumGroups = 4096;
constexpr uint32 kMaximumDagGroups = 65536;
constexpr uint32 kMaximumUnits = 262144;
// The DAG halves its triangles per level; deeper data is malformed.
constexpr uint32 kMaximumDepth = 64;
// A fine page is one unit; a coarse mesh is bounded by the file size only.
constexpr uint32 kMaximumPageVertices = 65535;
constexpr uint32 kMaximumCoarseVertices = static_cast<uint32>(kMaximumFileBytes / sizeof(Vertex));
constexpr uint32 kMaximumMaterials = 128;
constexpr uint32 kMaximumImages = 128;
constexpr uint32 kMaximumStringBytes = 4096;
constexpr float kMaximumComponent = 100000000.0f;
constexpr std::array<String ImportedMaterialData::*, 7> kTextureFields{
    &ImportedMaterialData::DiffuseTexture, &ImportedMaterialData::NormalTexture,
    &ImportedMaterialData::SpecularTexture, &ImportedMaterialData::EmissiveTexture,
    &ImportedMaterialData::OcclusionTexture, &ImportedMaterialData::RoughnessTexture,
    &ImportedMaterialData::MetallicTexture};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(Vertex) == 12 * sizeof(float));

class Writer
{
  public:
    template <typename T> requires std::is_trivially_copyable_v<T>
    bool Value(const T& value) { return Bytes(&value, sizeof(value)); }

    bool Bytes(const void* data, size_t size)
    {
        if (size > kMaximumFileBytes - m_Bytes.size())
            return false;
        const auto* first = static_cast<const uint8*>(data);
        if (size != 0)
            m_Bytes.insert(m_Bytes.end(), first, first + size);
        return true;
    }

    bool Text(const String& text)
    {
        return text.size() <= kMaximumStringBytes && Value(static_cast<uint32>(text.size())) &&
            Bytes(text.data(), text.size());
    }

    std::span<const uint8> Data() const { return m_Bytes; }

  private:
    Vector<uint8> m_Bytes;
};

class Reader
{
  public:
    explicit Reader(std::span<const uint8> bytes) : m_Bytes(bytes) {}

    template <typename T> requires std::is_trivially_copyable_v<T>
    bool Value(T& value) { return Bytes(&value, sizeof(value)); }

    bool Bytes(void* destination, size_t size)
    {
        if (size > Remaining())
            return false;
        if (size != 0)
            std::memcpy(destination, m_Bytes.data() + m_Cursor, size);
        m_Cursor += size;
        return true;
    }

    bool Text(String& text, uint32 maximum = kMaximumStringBytes)
    {
        uint32 size = 0;
        if (!Value(size) || size > maximum || size > Remaining())
            return false;
        text.assign(reinterpret_cast<const char*>(m_Bytes.data() + m_Cursor), size);
        m_Cursor += size;
        return true;
    }

    size_t Remaining() const { return m_Bytes.size() - m_Cursor; }

  private:
    std::span<const uint8> m_Bytes;
    size_t m_Cursor = 0;
};

bool ValidFloats(std::span<const float> values)
{
    return std::all_of(values.begin(), values.end(), [](float value)
        { return std::isfinite(value) && std::abs(value) <= kMaximumComponent; });
}

bool ValidTransform(std::span<const float, 16> transform)
{
    return ValidFloats(transform) && transform[3] == 0.0f && transform[7] == 0.0f &&
        transform[11] == 0.0f && transform[15] == 1.0f;
}

bool ValidMaterial(const ImportedMaterialData& material)
{
    return material.Name.size() <= kMaximumStringBytes && material.AlphaMode == AlphaMode::Opaque &&
        ValidFloats(material.DiffuseColor) && ValidFloats(material.SpecularColor) &&
        ValidFloats(material.EmissiveColor) &&
        ValidFloats(material.DiffuseTextureTransform) && ValidFloats(material.NormalTextureTransform) &&
        ValidFloats(material.SpecularTextureTransform) && ValidFloats(material.EmissiveTextureTransform) &&
        ValidFloats(material.OcclusionTextureTransform) && ValidFloats(material.RoughnessTextureTransform) &&
        ValidFloats(material.MetallicTextureTransform) &&
        std::isfinite(material.Shininess) && std::isfinite(material.Metallic) &&
        std::isfinite(material.Roughness) && std::isfinite(material.AlphaCutoff);
}

bool ValidTextureReferences(const Vector<ImportedMaterialData>& materials, const Vector<EmbeddedImage>& images)
{
    for (const ImportedMaterialData& material : materials)
    {
        for (const auto field : kTextureFields)
        {
            const String* reference = &(material.*field);
            if (reference->empty())
                continue;
            if (reference->size() > kMaximumStringBytes)
                return false;
            if (!GUID(*reference).IsNull())
                continue;
            bool embedded = false;
            for (size_t image = 0; image < images.size(); ++image)
                embedded = embedded || (*reference == std::format("{}{}", kEmbeddedTexturePrefix, image) &&
                                         images[image].HasContent());
            if (!embedded)
                return false;
        }
    }
    return true;
}

bool RebaseTextures(const ModelAsset& source, Vector<ImportedMaterialData>& materials, String& error)
{
    AssetManager* manager = AssetManager::GetThreadCurrent();
    for (ImportedMaterialData& material : materials)
    {
        if (!ValidMaterial(material))
        {
            error = "Cluster models require finite opaque materials";
            return false;
        }
        for (const auto field : kTextureFields)
        {
            String* reference = &(material.*field);
            if (reference->empty() || IsEmbeddedTextureRef(*reference) || !GUID(*reference).IsNull())
                continue;
            const auto path = std::filesystem::path(*reference);
            const GUID guid = manager ? manager->GetRegistry().GetAssetGUID(
                path.is_absolute() ? path : source.GetPath().parent_path() / path) : GUID::Null();
            if (guid.IsNull())
            {
                error = std::format("Register texture '{}' before cooking the cluster model", *reference);
                return false;
            }
            *reference = guid.ToString();
        }
    }
    if (!ValidTextureReferences(materials, source.GetEmbeddedImages()))
    {
        error = "Cluster material references a missing embedded image or invalid texture identity";
        return false;
    }
    return true;
}

bool WritePlacement(Writer& writer, const Mesh& mesh)
{
    if (!writer.Text(mesh.Name) || !writer.Value(mesh.MaterialIndex) ||
        !writer.Value(mesh.SourceNodeIndex) || !writer.Value(mesh.SourceNodeParentIndex) ||
        !writer.Value(mesh.SourceNodeTransform) || !writer.Value(mesh.SourceNodeLocalTransform) ||
        !writer.Value(static_cast<uint32>(mesh.ExtraPlacements.size())))
        return false;
    for (const MeshPlacement& placement : mesh.ExtraPlacements)
        if (!writer.Value(placement.SourceNodeIndex) || !writer.Value(placement.SourceNodeTransform) ||
            !writer.Value(placement.SourceNodeLocalTransform))
            return false;
    return writer.Value(mesh.MinBounds) && writer.Value(mesh.MaxBounds);
}

bool ReadPlacement(Reader& reader, Mesh& mesh, uint32& placements)
{
    uint32 count = 0;
    if (!reader.Text(mesh.Name) || !reader.Value(mesh.MaterialIndex) ||
        !reader.Value(mesh.SourceNodeIndex) || !reader.Value(mesh.SourceNodeParentIndex) ||
        !reader.Value(mesh.SourceNodeTransform) || !reader.Value(mesh.SourceNodeLocalTransform) ||
        !reader.Value(count) || count > 1024 || placements >= kMaximumGroups ||
        count > kMaximumGroups - placements - 1 ||
        !ValidTransform(mesh.SourceNodeTransform) || !ValidTransform(mesh.SourceNodeLocalTransform))
        return false;
    placements += count + 1;
    mesh.ExtraPlacements.resize(count);
    for (MeshPlacement& placement : mesh.ExtraPlacements)
        if (!reader.Value(placement.SourceNodeIndex) || !reader.Value(placement.SourceNodeTransform) ||
            !reader.Value(placement.SourceNodeLocalTransform) || !ValidTransform(placement.SourceNodeTransform) ||
            !ValidTransform(placement.SourceNodeLocalTransform))
            return false;
    if (!reader.Value(mesh.MinBounds) || !reader.Value(mesh.MaxBounds) ||
        !ValidFloats(mesh.MinBounds) || !ValidFloats(mesh.MaxBounds))
        return false;
    for (size_t axis = 0; axis < 3; ++axis)
        if (mesh.MinBounds[axis] > mesh.MaxBounds[axis])
            return false;
    return true;
}

// Finite vertices inside the mesh bounds, indices in range and positive-area triangles.
bool ValidGeometry(const Mesh& mesh)
{
    const auto vertices = static_cast<uint32>(mesh.Vertices.size());
    for (const Vertex& vertex : mesh.Vertices)
    {
        if (!ValidFloats(vertex.Position) || !ValidFloats(vertex.Normal) ||
            !ValidFloats(vertex.TexCoords) || !ValidFloats(vertex.Tangent))
            return false;
        for (size_t axis = 0; axis < 3; ++axis)
            if (vertex.Position[axis] < mesh.MinBounds[axis] || vertex.Position[axis] > mesh.MaxBounds[axis])
                return false;
    }
    if (!std::all_of(mesh.Indices.begin(), mesh.Indices.end(),
        [vertices](uint32 index) { return index < vertices; }))
        return false;
    for (size_t offset = 0; offset < mesh.Indices.size(); offset += 3)
    {
        const auto& first = mesh.Vertices[mesh.Indices[offset]].Position;
        const auto& second = mesh.Vertices[mesh.Indices[offset + 1]].Position;
        const auto& third = mesh.Vertices[mesh.Indices[offset + 2]].Position;
        const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
        const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
        if (Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared() <= 0.0f)
            return false;
    }
    return true;
}

bool ReadVertices(Reader& reader, uint32 vertices, Mesh& mesh)
{
    if (vertices == 0 || vertices > kMaximumCoarseVertices ||
        static_cast<uint64>(vertices) * sizeof(Vertex) > reader.Remaining())
        return false;
    mesh.Vertices.resize(vertices);
    if (!reader.Bytes(mesh.Vertices.data(), vertices * sizeof(Vertex)))
        return false;
    mesh.Indices.clear();
    return ValidGeometry(mesh);
}

uint64 EncodedPageBound(uint32 vertices, uint32 indices)
{
    return meshopt_encodeVertexBufferBound(vertices, sizeof(Vertex)) + meshopt_encodeIndexBufferBound(indices, vertices);
}

// Appends the encoded fine page and sets its Bytes and EncodedVertexBytes.
bool WriteEncodedPage(Writer& writer, const Mesh& mesh, MeshClusterLODManifest::Unit& unit)
{
    Vector<uint8> encoded(EncodedPageBound(unit.VertexCount, unit.IndexCount));
    const size_t vertexBytes = meshopt_encodeVertexBufferLevel(encoded.data(), encoded.size(), mesh.Vertices.data(),
                                                               mesh.Vertices.size(), sizeof(Vertex), kPageVertexLevel,
                                                               kPageVertexVersion);
    const size_t indexBytes = vertexBytes == 0 ? 0 : meshopt_encodeIndexBuffer(
        encoded.data() + vertexBytes, encoded.size() - vertexBytes, mesh.Indices.data(), mesh.Indices.size());
    if (indexBytes == 0)
        return false;
    unit.EncodedVertexBytes = static_cast<uint32>(vertexBytes);
    unit.Bytes = vertexBytes + indexBytes;
    return writer.Bytes(encoded.data(), static_cast<size_t>(unit.Bytes));
}

bool ValidBounds(const Mathematics::Vector3& low, const Mathematics::Vector3& high)
{
    const std::array<float, 6> bounds{low.x, low.y, low.z, high.x, high.y, high.z};
    return ValidFloats(bounds) && low.x <= high.x && low.y <= high.y && low.z <= high.z;
}

bool Contains(const MeshClusterLODManifest::Unit& unit, const float (&position)[3])
{
    return position[0] >= unit.MinBounds.x && position[0] <= unit.MaxBounds.x &&
        position[1] >= unit.MinBounds.y && position[1] <= unit.MaxBounds.y &&
        position[2] >= unit.MinBounds.z && position[2] <= unit.MaxBounds.z;
}

void SetUnitBounds(const MeshClusterLODManifest::Unit& unit, Mesh& mesh)
{
    for (size_t axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = unit.MinBounds[axis];
        mesh.MaxBounds[axis] = unit.MaxBounds[axis];
    }
}

bool WriteGroup(Writer& writer, const MeshClusterLODDagGroup& group)
{
    return writer.Value(group.Center) && writer.Value(group.Radius) && writer.Value(group.Error) &&
        writer.Value(group.Depth) && writer.Value(static_cast<uint32>(group.Terminal ? 1 : 0));
}

bool ReadGroup(Reader& reader, MeshClusterLODManifest::Group& group)
{
    uint32 terminal = 0;
    if (!reader.Value(group.Center) || !reader.Value(group.Radius) || !reader.Value(group.Error) ||
        !reader.Value(group.Depth) || !reader.Value(terminal) || terminal > 1)
        return false;
    group.Terminal = terminal != 0;
    const std::array<float, 5> values{group.Center.x, group.Center.y, group.Center.z, group.Radius, group.Error};
    return ValidFloats(values) && group.Radius >= 0.0f && group.Error >= 0.0f && group.Depth < kMaximumDepth;
}

// The unit record: group, producer (mesh-local), bounds, index count and kind, then the page
// record of a streamed unit or the coarse indices of a resident unit.
bool WriteUnitHeader(Writer& writer, uint32 group, uint32 producer, const MeshClusterLODManifest::Unit& unit, bool streamed)
{
    return writer.Value(group) && writer.Value(producer) && writer.Value(unit.MinBounds) && writer.Value(unit.MaxBounds) &&
        writer.Value(unit.IndexCount) && writer.Value(static_cast<uint32>(streamed ? 1 : 0));
}

bool WritePage(Writer& writer, const MeshClusterLODManifest::Unit& unit)
{
    return writer.Value(unit.Offset) && writer.Value(unit.Bytes) && writer.Value(unit.ContentHash) &&
        writer.Value(unit.EncodedVertexBytes) && writer.Value(unit.VertexCount);
}

// Pages follow each other in the fine store in unit order.
bool ReadPage(Reader& reader, uint64 storeBytes, uint64 expectedOffset, MeshClusterLODManifest::Unit& unit)
{
    if (!reader.Value(unit.Offset) || !reader.Value(unit.Bytes) || !reader.Value(unit.ContentHash) ||
        !reader.Value(unit.EncodedVertexBytes) || !reader.Value(unit.VertexCount))
        return false;
    return unit.Offset == expectedOffset && unit.Offset <= storeBytes && unit.Bytes != 0 &&
        unit.Bytes <= storeBytes - unit.Offset && unit.VertexCount != 0 && unit.VertexCount <= kMaximumPageVertices &&
        static_cast<uint64>(unit.VertexCount) * sizeof(Vertex) + static_cast<uint64>(unit.IndexCount) * sizeof(uint32) <=
            kMaximumFileBytes &&
        unit.EncodedVertexBytes != 0 && unit.EncodedVertexBytes < unit.Bytes &&
        unit.EncodedVertexBytes <= meshopt_encodeVertexBufferBound(unit.VertexCount, sizeof(Vertex)) &&
        unit.Bytes <= EncodedPageBound(unit.VertexCount, unit.IndexCount);
}

// Coarse indices of a resident unit: in range, positive area and inside the unit bounds.
bool ReadResidentIndices(Reader& reader, const Mesh& coarse, const MeshClusterLODManifest::Unit& unit,
                         Vector<uint32>& indices)
{
    if (static_cast<uint64>(unit.IndexCount) * sizeof(uint32) > reader.Remaining())
        return false;
    indices.resize(unit.IndexCount);
    if (!reader.Bytes(indices.data(), indices.size() * sizeof(uint32)))
        return false;
    for (size_t offset = 0; offset < indices.size(); offset += 3)
    {
        for (size_t corner = 0; corner < 3; ++corner)
            if (indices[offset + corner] >= coarse.Vertices.size() ||
                !Contains(unit, coarse.Vertices[indices[offset + corner]].Position))
                return false;
        const auto& first = coarse.Vertices[indices[offset]].Position;
        const auto& second = coarse.Vertices[indices[offset + 1]].Position;
        const auto& third = coarse.Vertices[indices[offset + 2]].Position;
        const Mathematics::Vector3 edgeA{second[0] - first[0], second[1] - first[1], second[2] - first[2]};
        const Mathematics::Vector3 edgeB{third[0] - first[0], third[1] - first[1], third[2] - first[2]};
        if (Mathematics::Vector3::Cross(edgeA, edgeB).LengthSquared() <= 0.0f)
            return false;
    }
    return true;
}

// A unit is resident when a uniform cut at or over the resident depth draws it.
bool IsResident(uint32 groupDepth, bool groupTerminal, uint32 residentDepth)
{
    return groupTerminal || groupDepth >= residentDepth;
}

// The first DAG level whose uniform cut has at most `ratio` of the source triangles.
uint32 ChooseResidentDepth(const MeshClusterLODDag& dag, size_t sourceTriangles, float ratio)
{
    uint32 maximumDepth = 0;
    for (const MeshClusterLODDagGroup& group : dag.Groups)
        maximumDepth = std::max(maximumDepth, group.Depth);
    Vector<uint64> triangles(maximumDepth + 1, 0);
    for (const MeshClusterLODDagUnit& unit : dag.Units)
    {
        const MeshClusterLODDagGroup& group = dag.Groups[unit.Group];
        for (uint32 depth = group.Depth; depth <= (group.Terminal ? maximumDepth : group.Depth); ++depth)
            triangles[depth] += unit.Indices.size() / 3;
    }
    const auto limit = static_cast<uint64>(static_cast<double>(sourceTriangles) * ratio);
    for (uint32 depth = 1; depth <= maximumDepth; ++depth)
        if (triangles[depth] <= limit)
            return depth;
    return maximumDepth;
}

// Builds LOD0 (the resident cut) and every coarser uniform cut of one mesh from its resident
// units, records each unit's first level and range, and sets the level errors relative to the
// largest bounds axis, the unit of Mesh::ExtraLODErrors.
bool BuildLevels(MeshClusterLODManifest& manifest, const MeshClusterLODManifest::MeshRange& range,
                 const Vector<Vector<uint32>>& residentIndices, Mesh& coarse)
{
    uint32 maximumDepth = 0;
    for (uint32 group = range.FirstGroup; group < range.FirstGroup + range.GroupCount; ++group)
        maximumDepth = std::max(maximumDepth, manifest.Groups[group].Depth);
    if (range.ResidentDepth > maximumDepth)
        return false;
    const float extent = std::max({coarse.MaxBounds[0] - coarse.MinBounds[0], coarse.MaxBounds[1] - coarse.MinBounds[1],
                                   coarse.MaxBounds[2] - coarse.MinBounds[2]});
    Vector<bool> placed(range.UnitCount, false);
    coarse.ExtraLODs.clear();
    coarse.ExtraLODErrors.clear();
    coarse.ExtraLODSloppy.clear();
    for (uint32 depth = range.ResidentDepth; depth <= maximumDepth; ++depth)
    {
        Vector<uint32>& indices = depth == range.ResidentDepth ? coarse.Indices : coarse.ExtraLODs.emplace_back();
        float error = 0.0f;
        for (uint32 local = 0; local < range.UnitCount; ++local)
        {
            auto& unit = manifest.Units[range.FirstUnit + local];
            const auto& group = manifest.Groups[unit.Group];
            if (!IsMeshClusterLODUnitInLevel(group.Depth, group.Terminal, depth))
                continue;
            if (unit.IsStreamed() || indices.size() > std::numeric_limits<uint32>::max() - unit.IndexCount)
                return false;
            if (!placed[local])
            {
                placed[local] = true;
                unit.Level = depth - range.ResidentDepth;
                unit.FirstIndex = static_cast<uint32>(indices.size());
            }
            if (unit.Producer != kMeshClusterLODSourceGroup)
                error = std::max(error, manifest.Groups[unit.Producer].Error);
            indices.insert(indices.end(), residentIndices[local].begin(), residentIndices[local].end());
        }
        if (indices.empty())
            return false;
        if (depth != range.ResidentDepth)
        {
            coarse.ExtraLODErrors.push_back(extent > 0.0f ? error / extent : 0.0f);
            coarse.ExtraLODSloppy.push_back(0);
        }
    }
    for (uint32 local = 0; local < range.UnitCount; ++local)
        if (placed[local] == manifest.Units[range.FirstUnit + local].IsStreamed())
            return false;
    return true;
}

// Checks the DAG edges of one mesh and derives the unit and produced-unit ranges of its groups.
// A unit's group is one level above its producer, never under a terminal producer, and its
// error is at least the producer's, so a cut chosen top-down is crack-free and monotone.
bool LinkGroups(MeshClusterLODManifest& manifest, const MeshClusterLODManifest::MeshRange& range)
{
    for (uint32 local = 0; local < range.UnitCount; ++local)
    {
        const auto& unit = manifest.Units[range.FirstUnit + local];
        const auto& group = manifest.Groups[unit.Group];
        if (local != 0)
        {
            const auto& previous = manifest.Units[range.FirstUnit + local - 1];
            if (previous.Group > unit.Group || (previous.Group == unit.Group && previous.Producer >= unit.Producer))
                return false;
        }
        if (unit.Producer == kMeshClusterLODSourceGroup)
        {
            if (group.Depth != 0)
                return false;
            continue;
        }
        const auto& producer = manifest.Groups[unit.Producer];
        if (producer.Terminal || group.Depth != producer.Depth + 1 || (!group.Terminal && group.Error < producer.Error))
            return false;
    }
    Vector<uint32> produced(range.GroupCount, 0);
    for (uint32 local = 0; local < range.UnitCount; ++local)
    {
        const auto& unit = manifest.Units[range.FirstUnit + local];
        auto& group = manifest.Groups[unit.Group];
        if (group.UnitCount == 0)
            group.FirstUnit = range.FirstUnit + local;
        ++group.UnitCount;
        if (unit.Producer != kMeshClusterLODSourceGroup)
            ++produced[unit.Producer - range.FirstGroup];
    }
    auto first = static_cast<uint32>(manifest.ProducedUnits.size());
    for (uint32 local = 0; local < range.GroupCount; ++local)
    {
        auto& group = manifest.Groups[range.FirstGroup + local];
        if (group.UnitCount == 0 || group.Terminal != (produced[local] == 0))
            return false;
        group.FirstProduced = first;
        first += produced[local];
    }
    manifest.ProducedUnits.resize(first);
    for (uint32 local = 0; local < range.UnitCount; ++local)
    {
        const auto& unit = manifest.Units[range.FirstUnit + local];
        if (unit.Producer == kMeshClusterLODSourceGroup)
            continue;
        auto& producer = manifest.Groups[unit.Producer];
        manifest.ProducedUnits[producer.FirstProduced + producer.ProducedCount++] = range.FirstUnit + local;
    }
    return true;
}

bool Publish(const std::filesystem::path& path, std::span<const uint8> bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    const auto temporary = FileSystem::MakeTemporarySiblingPath(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    if (IsAssetDecodeCancelled() || !FileSystem::PublishFile(temporary, path))
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

String StoreName(uint64 hash) { return std::format("{:016x}.pages", hash); }

bool StoreMatches(SharedFileReader& reader, std::span<const uint8> expected)
{
    if (reader.Size() != static_cast<int64>(expected.size()))
        return false;
    std::array<uint8, 16384> chunk;
    size_t cursor = 0;
    while (cursor < expected.size())
    {
        const size_t size = std::min(chunk.size(), expected.size() - cursor);
        if (reader.Read(chunk.data(), size) != static_cast<int64>(size) ||
            !std::equal(chunk.begin(), chunk.begin() + size, expected.begin() + cursor, expected.begin() + cursor + size))
            return false;
        cursor += size;
    }
    uint8 extra = 0;
    return reader.Read(&extra, 1) == 0;
}

class MaterialSyntax final : public nlohmann::json_sax<nlohmann::json>
{
  public:
    bool null() override { return Count(); }
    bool boolean(bool) override { return Count(); }
    bool number_integer(number_integer_t) override { return Count(); }
    bool number_unsigned(number_unsigned_t) override { return Count(); }
    bool number_float(number_float_t, const string_t&) override { return Count(); }
    bool string(string_t& text) override { return text.size() <= kMaximumStringBytes && Count(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(size_t) override { return ++m_Depth <= 8 && Count(); }
    bool key(string_t& text) override { return text.size() <= kMaximumStringBytes && Count(); }
    bool end_object() override { --m_Depth; return true; }
    bool start_array(size_t) override { return ++m_Depth <= 8 && Count(); }
    bool end_array() override { --m_Depth; return true; }
    bool parse_error(size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }

  private:
    bool Count() { return ++m_Values <= 65536; }
    size_t m_Depth = 0;
    size_t m_Values = 0;
};

bool ReadMaterials(Reader& reader, Vector<ImportedMaterialData>& materials)
{
    String text;
    if (!reader.Text(text, kMaximumMaterialBytes))
        return false;
    MaterialSyntax syntax;
    if (!nlohmann::json::sax_parse(text.begin(), text.end(), &syntax))
        return false;
    auto decoded = DeserializeImportedMaterials(text);
    if (!decoded || decoded->size() > kMaximumMaterials ||
        !std::all_of(decoded->begin(), decoded->end(), ValidMaterial))
        return false;
    materials = std::move(*decoded);
    return true;
}

bool ReadImages(Reader& reader, Vector<EmbeddedImage>& images)
{
    uint32 count = 0;
    if (!reader.Value(count) || count > kMaximumImages)
        return false;
    images.resize(count);
    uint64 remaining = kMaximumImageBytes;
    for (EmbeddedImage& image : images)
    {
        GUID::Data guid;
        uint32 size = 0;
        if (!reader.Text(image.MimeType) || !reader.Value(guid) || !reader.Value(size) ||
            size > remaining || size > reader.Remaining())
            return false;
        image.AssetGuid = GUID(guid);
        image.Data = ByteBuffer(size);
        if (!reader.Bytes(image.Data.Data(), size))
            return false;
        remaining -= size;
    }
    return true;
}

bool WriteImages(Writer& writer, const Vector<EmbeddedImage>& images)
{
    if (images.size() > kMaximumImages || !writer.Value(static_cast<uint32>(images.size())))
        return false;
    uint64 remaining = kMaximumImageBytes;
    for (const EmbeddedImage& image : images)
    {
        if (image.Data.Size() > remaining || !writer.Text(image.MimeType) ||
            !writer.Value(image.AssetGuid.GetData()) || !writer.Value(static_cast<uint32>(image.Data.Size())) ||
            !writer.Bytes(image.Data.Data(), image.Data.Size()))
            return false;
        remaining -= image.Data.Size();
    }
    return true;
}

} // namespace

bool DecodeMeshClusterLODAsset(std::span<const uint8> bytes, MeshClusterLODAssetData& output, String& error)
{
    error = "Cluster model is truncated, invalid, or exceeds its limits";
    if (bytes.size() > kMaximumFileBytes)
        return false;
    Reader reader(bytes);
    uint64 magic = 0;
    uint32 version = 0;
    MeshClusterLODAssetData decoded;
    auto& manifest = decoded.Manifest;
    if (!reader.Value(magic) || magic != kMagic || !reader.Value(version) || version != kVersion ||
        !reader.Value(manifest.Incarnation) ||
        Hashing::Fnv1a64(bytes.data() + bytes.size() - reader.Remaining(), reader.Remaining()) != manifest.Incarnation)
        return false;
    if (!reader.Value(manifest.StoreHash) || !reader.Value(manifest.StoreBytes) ||
        manifest.StoreBytes > kMaximumFileBytes)
    {
        error = "Cluster fine store size exceeds its limit";
        return false;
    }
    if (!ReadMaterials(reader, decoded.Materials) || !ReadImages(reader, decoded.Images) ||
        !ValidTextureReferences(decoded.Materials, decoded.Images))
    {
        error = "Cluster material or image metadata is invalid";
        return false;
    }
    uint32 meshes = 0;
    if (!reader.Value(meshes) || meshes == 0 || meshes > kMaximumGroups)
        return false;
    decoded.CoarseMeshes.resize(meshes);
    manifest.Meshes.resize(meshes);
    uint64 offset = 0;
    uint32 placements = 0;
    FlatSet<String> names;
    names.Reserve(meshes);
    Vector<Vector<uint32>> residentIndices;
    for (uint32 meshIndex = 0; meshIndex < meshes; ++meshIndex)
    {
        Mesh& mesh = decoded.CoarseMeshes[meshIndex];
        auto& range = manifest.Meshes[meshIndex];
        uint32 vertices = 0;
        if (!ReadPlacement(reader, mesh, placements) || !names.Insert(mesh.Name) ||
            (!decoded.Materials.empty() && mesh.MaterialIndex >= decoded.Materials.size()) ||
            (decoded.Materials.empty() && mesh.MaterialIndex != 0))
        {
            error = std::format("Cluster mesh {} has invalid placement, bounds, name, or material", meshIndex);
            return false;
        }
        if (!reader.Value(range.ResidentDepth) || range.ResidentDepth >= kMaximumDepth || !reader.Value(vertices) ||
            !ReadVertices(reader, vertices, mesh))
        {
            error = std::format("Cluster mesh {} has invalid coarse vertices", meshIndex);
            return false;
        }
        uint32 groups = 0;
        if (!reader.Value(groups) || groups == 0 || groups > kMaximumDagGroups - manifest.Groups.size())
            return false;
        range.FirstGroup = static_cast<uint32>(manifest.Groups.size());
        range.GroupCount = groups;
        for (uint32 group = 0; group < groups; ++group)
        {
            MeshClusterLODManifest::Group decodedGroup;
            if (!ReadGroup(reader, decodedGroup))
            {
                error = std::format("Cluster mesh {} group {} is invalid", meshIndex, group);
                return false;
            }
            decodedGroup.Mesh = meshIndex;
            manifest.Groups.push_back(decodedGroup);
        }
        uint32 units = 0;
        if (!reader.Value(units) || units == 0 || units > kMaximumUnits - manifest.Units.size())
            return false;
        range.FirstUnit = static_cast<uint32>(manifest.Units.size());
        range.UnitCount = units;
        residentIndices.assign(units, {});
        for (uint32 local = 0; local < units; ++local)
        {
            MeshClusterLODManifest::Unit unit;
            uint32 group = 0;
            uint32 producer = 0;
            uint32 streamed = 0;
            unit.Mesh = meshIndex;
            const bool header = reader.Value(group) && reader.Value(producer) && reader.Value(unit.MinBounds) &&
                reader.Value(unit.MaxBounds) && reader.Value(unit.IndexCount) && reader.Value(streamed) &&
                group < groups && (producer == kMeshClusterLODSourceGroup || producer < groups) && streamed <= 1 &&
                unit.IndexCount != 0 && unit.IndexCount % 3 == 0 && ValidBounds(unit.MinBounds, unit.MaxBounds);
            if (!header)
            {
                error = std::format("Cluster mesh {} unit {} has an invalid record", meshIndex, local);
                return false;
            }
            unit.Group = range.FirstGroup + group;
            unit.Producer = producer == kMeshClusterLODSourceGroup ? producer : range.FirstGroup + producer;
            const auto& owner = manifest.Groups[unit.Group];
            const bool payload = streamed != 0
                ? ReadPage(reader, manifest.StoreBytes, offset, unit)
                : ReadResidentIndices(reader, mesh, unit, residentIndices[local]);
            if (!payload || (streamed != 0) == IsResident(owner.Depth, owner.Terminal, range.ResidentDepth))
            {
                error = std::format("Cluster mesh {} unit {} has an invalid page or resident range", meshIndex, local);
                return false;
            }
            offset += unit.Bytes;
            manifest.Units.push_back(unit);
        }
        if (!LinkGroups(manifest, range) || !BuildLevels(manifest, range, residentIndices, mesh))
        {
            error = std::format("Cluster mesh {} has an invalid DAG or LOD chain", meshIndex);
            return false;
        }
    }
    if (offset != manifest.StoreBytes || reader.Remaining() != 0 || IsAssetDecodeCancelled())
        return false;
    FlatSet<GUID> dependencies;
    for (const ImportedMaterialData& material : decoded.Materials)
        for (const auto field : kTextureFields)
        {
            const GUID guid(material.*field);
            if (!guid.IsNull())
                dependencies.Insert(guid);
        }
    for (const EmbeddedImage& image : decoded.Images)
        if (!image.AssetGuid.IsNull())
            dependencies.Insert(image.AssetGuid);
    manifest.TextureDependencies.assign(dependencies.begin(), dependencies.end());
    output = std::move(decoded);
    error.clear();
    return true;
}

bool ModelAsset::LoadClusterLOD(std::span<const uint8> data)
{
    MeshClusterLODAssetData decoded;
    String error;
    if (!DecodeMeshClusterLODAsset(data, decoded, error))
    {
        Logger::Log::Error("Cannot load cluster model '{}': {}", GetPath().string(), error);
        return false;
    }
    m_Meshes = std::move(decoded.CoarseMeshes);
    m_Materials = std::move(decoded.Materials);
    m_EmbeddedImages = std::move(decoded.Images);
    m_ClusterLODManifest = std::make_shared<const MeshClusterLODManifest>(std::move(decoded.Manifest));
    return true;
}

uint64 ModelAsset::GetMaximumReadBytes() const
{
    return m_ClusterLODManifest || FormatFromPath(GetPath()) == ModelFormat::ClusterLOD
        ? MeshClusterLODManifest::kMaximumFileBytes : std::numeric_limits<uint64>::max();
}

bool ModelAsset::ReloadFromData(std::span<const uint8> data)
{
    if (!m_ClusterLODManifest && FormatFromPath(GetPath()) != ModelFormat::ClusterLOD)
        return Asset::ReloadFromData(data);
    MeshClusterLODAssetData decoded;
    String error;
    if (!m_ClusterLODManifest || !DecodeMeshClusterLODAsset(data, decoded, error) ||
        decoded.Manifest.Incarnation != m_ClusterLODManifest->Incarnation)
    {
        Logger::Log::Error("Cluster model '{}' cannot replace its live groups; reload the scene after recooking", GetPath().string());
        return false;
    }
    return true;
}

namespace
{

// The source vertices that resident units use, in first-use order, and the source-to-coarse map.
Mesh BuildResidentVertices(const Mesh& source, const MeshClusterLODDag& dag, uint32 residentDepth, Vector<uint32>& remap)
{
    Mesh coarse;
    CopyMeshClusterLODPlacement(source, coarse);
    remap.assign(source.Vertices.size(), kMeshClusterLODUnusedVertex);
    for (const MeshClusterLODDagUnit& unit : dag.Units)
    {
        const MeshClusterLODDagGroup& group = dag.Groups[unit.Group];
        if (!IsResident(group.Depth, group.Terminal, residentDepth))
            continue;
        for (const uint32 index : unit.Indices)
            if (remap[index] == kMeshClusterLODUnusedVertex)
            {
                remap[index] = static_cast<uint32>(coarse.Vertices.size());
                coarse.Vertices.push_back(source.Vertices[index]);
            }
    }
    // The coarse bounds cover the source, so they cover every streamed unit too.
    Mathematics::AABB bounds = Mathematics::AABB::Empty();
    for (const MeshClusterLODDagUnit& unit : dag.Units)
        for (const uint32 index : unit.Indices)
        {
            const auto& position = source.Vertices[index].Position;
            bounds.Expand({position[0], position[1], position[2]});
        }
    for (size_t axis = 0; axis < 3; ++axis)
    {
        coarse.MinBounds[axis] = bounds.min[axis];
        coarse.MaxBounds[axis] = bounds.max[axis];
    }
    return coarse;
}

void SetBoundsOfIndices(const Mesh& source, std::span<const uint32> indices, MeshClusterLODManifest::Unit& unit)
{
    Mathematics::AABB bounds = Mathematics::AABB::Empty();
    for (const uint32 index : indices)
    {
        const auto& position = source.Vertices[index].Position;
        bounds.Expand({position[0], position[1], position[2]});
    }
    unit.MinBounds = bounds.min;
    unit.MaxBounds = bounds.max;
}

} // namespace

bool CookMeshClusterLODAsset(const ModelAsset& source, const std::filesystem::path& output,
                            const MeshClusterLODConfig& config, String& error)
{
    error = "Cluster cooking requires a loaded static model and a .gecluster output path";
    if (!source.IsLoaded() || source.GetClusterLODManifest() || source.GetMeshCount() == 0 || source.GetMeshCount() > kMaximumGroups ||
        source.HasAnimations() ||
        source.GetMaterials().size() > kMaximumMaterials ||
        ModelAsset::FormatFromPath(output) != ModelFormat::ClusterLOD)
        return false;
    Vector<ImportedMaterialData> materials = source.GetMaterials();
    if (!RebaseTextures(source, materials, error))
        return false;
    const String materialText = SerializeImportedMaterials(materials);
    if (materialText.size() > kMaximumMaterialBytes)
    {
        error = "Cluster material metadata exceeds one MiB";
        return false;
    }
    Writer fine;
    Writer meshes;
    FlatSet<String> names;
    uint32 meshCount = 0;
    uint32 placements = 0;
    uint64 groupCount = 0;
    uint64 unitCount = 0;
    uint64 geometryBytes = 0;
    uint64 metadataBytes = 0;
    Vector<uint32> remap;
    Vector<uint32> pageRemap;
    Vector<uint32> pageVertices;
    for (const Mesh& mesh : source.GetMeshes())
    {
        if (!ValidTransform(mesh.SourceNodeTransform) || !ValidTransform(mesh.SourceNodeLocalTransform) ||
            (!materials.empty() && mesh.MaterialIndex >= materials.size()) ||
            (materials.empty() && mesh.MaterialIndex != 0) ||
            std::any_of(mesh.ExtraPlacements.begin(), mesh.ExtraPlacements.end(), [](const MeshPlacement& placement)
                { return !ValidTransform(placement.SourceNodeTransform) || !ValidTransform(placement.SourceNodeLocalTransform); }))
        {
            error = "Cluster mesh has an invalid material index or non-affine placement";
            return false;
        }
        MeshClusterLODDag dag;
        if (!GenerateMeshClusterLODDag(mesh, config, dag, error))
            return false;
        // config.MaximumGroups bounds the DAG groups of the whole model, as it does each mesh.
        if (placements >= kMaximumGroups || mesh.ExtraPlacements.size() > kMaximumGroups - placements - 1 ||
            dag.Groups.size() > std::min<uint64>(kMaximumDagGroups, config.MaximumGroups) - groupCount ||
            dag.Units.size() > kMaximumUnits - unitCount)
        {
            error = "Cluster model exceeds its placement, group or unit limit";
            return false;
        }
        placements += static_cast<uint32>(mesh.ExtraPlacements.size()) + 1;
        groupCount += dag.Groups.size();
        unitCount += dag.Units.size();
        const uint32 residentDepth = ChooseResidentDepth(dag, mesh.Indices.size() / 3, config.ResidentRatio);
        Mesh coarse = BuildResidentVertices(mesh, dag, residentDepth, remap);
        if (!names.Insert(coarse.Name))
            coarse.Name = std::format("{} {}", mesh.Name, meshCount);
        if (coarse.Name != mesh.Name && !names.Insert(coarse.Name))
        {
            error = "Cluster mesh names are not unique";
            return false;
        }
        const uint64 meshMetadata = sizeof(Mesh) + coarse.Name.size() +
            dag.Groups.size() * sizeof(MeshClusterLODManifest::Group) +
            dag.Units.size() * sizeof(MeshClusterLODManifest::Unit) + coarse.ExtraPlacements.size() * sizeof(MeshPlacement);
        if (meshMetadata > config.MaximumMetadataBytes - metadataBytes)
        {
            error = "Cluster model exceeds the group metadata limit";
            return false;
        }
        metadataBytes += meshMetadata;
        if (!WritePlacement(meshes, coarse) || !meshes.Value(residentDepth) ||
            !meshes.Value(static_cast<uint32>(coarse.Vertices.size())) ||
            !meshes.Bytes(coarse.Vertices.data(), coarse.Vertices.size() * sizeof(Vertex)) ||
            !meshes.Value(static_cast<uint32>(dag.Groups.size())))
        {
            error = "Cluster groups exceed their serialized file limit";
            return false;
        }
        for (const MeshClusterLODDagGroup& group : dag.Groups)
            if (!WriteGroup(meshes, group))
            {
                error = "Cluster groups exceed their serialized file limit";
                return false;
            }
        if (!meshes.Value(static_cast<uint32>(dag.Units.size())))
        {
            error = "Cluster units exceed their serialized file limit";
            return false;
        }
        geometryBytes += coarse.Vertices.size() * sizeof(Vertex);
        pageRemap.assign(mesh.Vertices.size(), kMeshClusterLODUnusedVertex);
        for (const MeshClusterLODDagUnit& dagUnit : dag.Units)
        {
            const MeshClusterLODDagGroup& group = dag.Groups[dagUnit.Group];
            const bool streamed = !IsResident(group.Depth, group.Terminal, residentDepth);
            MeshClusterLODManifest::Unit unit;
            unit.IndexCount = static_cast<uint32>(dagUnit.Indices.size());
            SetBoundsOfIndices(mesh, dagUnit.Indices, unit);
            Mesh page;
            if (streamed)
            {
                page = CompactMeshClusterLODTriangles(mesh, dagUnit.Indices, pageRemap, pageVertices);
                unit.VertexCount = static_cast<uint32>(page.Vertices.size());
            }
            const uint64 bytes = streamed ? page.Vertices.size() * sizeof(Vertex) + page.Indices.size() * sizeof(uint32)
                                          : dagUnit.Indices.size() * sizeof(uint32);
            if (bytes > config.MaximumGeometryBytes - std::min(geometryBytes, config.MaximumGeometryBytes) ||
                unit.VertexCount > kMaximumPageVertices)
            {
                error = "Cluster model exceeds the total geometry payload limit";
                return false;
            }
            geometryBytes += bytes;
            if (!WriteUnitHeader(meshes, dagUnit.Group, dagUnit.Producer, unit, streamed))
            {
                error = "Cluster units exceed their serialized file limit";
                return false;
            }
            if (!streamed)
            {
                for (const uint32 index : dagUnit.Indices)
                    if (!meshes.Value(remap[index]))
                    {
                        error = "Cluster units exceed their serialized file limit";
                        return false;
                    }
                continue;
            }
            unit.Offset = fine.Data().size();
            if (!WriteEncodedPage(fine, page, unit))
            {
                error = "Cluster fine store exceeds its serialized file limit";
                return false;
            }
            const auto payload = fine.Data().subspan(static_cast<size_t>(unit.Offset), static_cast<size_t>(unit.Bytes));
            unit.ContentHash = Hashing::Fnv1a64(payload.data(), payload.size());
            if (!WritePage(meshes, unit))
            {
                error = "Cluster units exceed their serialized file limit";
                return false;
            }
        }
        ++meshCount;
    }
    const uint64 storeHash = Hashing::Fnv1a64(fine.Data().data(), fine.Data().size());
    Writer body;
    if (!body.Value(storeHash) || !body.Value(static_cast<uint64>(fine.Data().size())) ||
        !body.Value(static_cast<uint32>(materialText.size())) || !body.Bytes(materialText.data(), materialText.size()) ||
        !WriteImages(body, source.GetEmbeddedImages()) || !body.Value(meshCount) || !body.Bytes(meshes.Data().data(), meshes.Data().size()))
    {
        error = "Cluster model exceeds its serialized file or image limit";
        return false;
    }
    Writer file;
    const uint64 incarnation = Hashing::Fnv1a64(body.Data().data(), body.Data().size());
    if (!file.Value(kMagic) || !file.Value(kVersion) || !file.Value(incarnation) ||
        !file.Bytes(body.Data().data(), body.Data().size()))
    {
        error = "Cluster coarse model exceeds its serialized file limit";
        return false;
    }
    MeshClusterLODAssetData roundtrip;
    if (!DecodeMeshClusterLODAsset(file.Data(), roundtrip, error))
        return false;
    const auto store = output.parent_path() / ".cluster" / StoreName(storeHash);
    std::error_code filesystemError;
    std::filesystem::create_directories(store.parent_path(), filesystemError);
    if (filesystemError)
    {
        error = "Cannot create the cluster store directory";
        return false;
    }
    auto lockPath = store;
    lockPath += ".lock";
    FileSystem::ScopedFileLock storeLock(lockPath);
    if (!storeLock.IsLocked())
    {
        error = std::format("Cannot lock the cluster store: {}", storeLock.FailureReason());
        return false;
    }
    const bool exists = std::filesystem::exists(store, filesystemError);
    if (filesystemError)
    {
        error = "Cannot inspect the cluster fine store destination";
        return false;
    }
    if (exists)
    {
        SharedFileReader existing(store);
        if (!StoreMatches(existing, fine.Data()))
        {
            error = "Cluster store identity collides with different existing bytes";
            return false;
        }
    }
    else if (!Publish(store, fine.Data()))
    {
        error = "Cannot publish the cluster fine store";
        return false;
    }
    if (!Publish(output, file.Data()))
    {
        error = "Cannot publish the cluster coarse model or cooking was cancelled";
        return false;
    }
    error.clear();
    return true;
}

bool DecodeMeshClusterLODFinePage(std::span<const uint8> bytes, const MeshClusterLODManifest::Unit& unit,
                                 const Mesh& coarse, Mesh& output, String& error)
{
    error.clear();
    if (!unit.IsStreamed() || bytes.size() > kMaximumFileBytes || bytes.size() != unit.Bytes ||
        unit.EncodedVertexBytes >= bytes.size() || Hashing::Fnv1a64(bytes.data(), bytes.size()) != unit.ContentHash)
    {
        error = "Cluster fine page is truncated or has the wrong checksum";
        return false;
    }
    const auto vertexStream = bytes.first(unit.EncodedVertexBytes);
    const auto indexStream = bytes.subspan(unit.EncodedVertexBytes);
    Mesh decoded;
    CopyMeshClusterLODPlacement(coarse, decoded);
    SetUnitBounds(unit, decoded);
    decoded.Vertices.resize(unit.VertexCount);
    decoded.Indices.resize(unit.IndexCount);
    // The decoders accept untrusted input; ValidGeometry refuses garbage output.
    if (meshopt_decodeVertexBuffer(decoded.Vertices.data(), unit.VertexCount, sizeof(Vertex), vertexStream.data(),
                                   vertexStream.size()) != 0 ||
        meshopt_decodeIndexBuffer(decoded.Indices.data(), unit.IndexCount, sizeof(uint32), indexStream.data(),
                                  indexStream.size()) != 0 ||
        !ValidGeometry(decoded) || IsAssetDecodeCancelled())
    {
        error = "Cluster fine page has invalid geometry or was cancelled";
        return false;
    }
    output = std::move(decoded);
    return true;
}

Mesh ExtractMeshClusterLODResidentUnit(const Mesh& coarse, const MeshClusterLODManifest::Unit& unit)
{
    const Vector<uint32>& level = unit.Level == 0 ? coarse.Indices : coarse.ExtraLODs[unit.Level - 1];
    Vector<uint32> remap(coarse.Vertices.size(), kMeshClusterLODUnusedVertex);
    Vector<uint32> sourceVertices;
    Mesh result = CompactMeshClusterLODTriangles(
        coarse, std::span<const uint32>(level.data() + unit.FirstIndex, unit.IndexCount), remap, sourceVertices);
    SetUnitBounds(unit, result);
    return result;
}

std::filesystem::path ResolveMeshClusterLODStore(const std::filesystem::path& modelPath,
                                                const std::filesystem::path& assetRoot,
                                                const MeshClusterLODManifest& manifest)
{
    const auto sibling = modelPath.parent_path() / ".cluster" / StoreName(manifest.StoreHash);
    std::error_code error;
    if (std::filesystem::is_regular_file(sibling, error))
        return sibling;
    return assetRoot.empty() ? sibling : assetRoot / ".cluster" / StoreName(manifest.StoreHash);
}

} // namespace GameEngine
