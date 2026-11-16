#include "Assets/ClusterLOD/MeshClusterLODAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/ImportedMaterialCache.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/ScopedFileLock.h"
#include "Logger/Logger.h"
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
constexpr uint32 kVersion = 3;
// The newest meshopt vertex codec version and its default compression level. The index codec
// uses the library default, version 1, the newest.
constexpr int kPageVertexVersion = 1;
constexpr int kPageVertexLevel = 2;
constexpr uint64 kMaximumFileBytes = MeshClusterLODManifest::kMaximumFileBytes;
constexpr uint64 kMaximumImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint32 kMaximumMaterialBytes = 1024u * 1024u;
constexpr uint32 kMaximumGroups = 4096;
// A fine page is one cluster group; a coarse mesh is bounded by the file size only.
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

bool ReadGeometry(Reader& reader, uint32 vertices, uint32 indices, uint32 maximumVertices, Mesh& mesh)
{
    if (vertices == 0 || vertices > maximumVertices || indices == 0 || indices % 3 != 0 ||
        static_cast<uint64>(vertices) * sizeof(Vertex) + static_cast<uint64>(indices) * sizeof(uint32) > reader.Remaining())
        return false;
    mesh.Vertices.resize(vertices);
    mesh.Indices.resize(indices);
    return reader.Bytes(mesh.Vertices.data(), vertices * sizeof(Vertex)) &&
        reader.Bytes(mesh.Indices.data(), indices * sizeof(uint32)) && ValidGeometry(mesh);
}

bool WriteGeometry(Writer& writer, const Mesh& mesh)
{
    return writer.Bytes(mesh.Vertices.data(), mesh.Vertices.size() * sizeof(Vertex)) &&
        writer.Bytes(mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32));
}

uint64 EncodedPageBound(uint32 vertices, uint32 indices)
{
    return meshopt_encodeVertexBufferBound(vertices, sizeof(Vertex)) + meshopt_encodeIndexBufferBound(indices, vertices);
}

// Appends the encoded fine page and sets its Bytes and EncodedVertexBytes.
bool WriteEncodedPage(Writer& writer, const Mesh& mesh, MeshClusterLODManifest::Page& page)
{
    Vector<uint8> encoded(EncodedPageBound(page.VertexCount, page.IndexCount));
    const size_t vertexBytes = meshopt_encodeVertexBufferLevel(encoded.data(), encoded.size(), mesh.Vertices.data(),
                                                               mesh.Vertices.size(), sizeof(Vertex), kPageVertexLevel,
                                                               kPageVertexVersion);
    const size_t indexBytes = vertexBytes == 0 ? 0 : meshopt_encodeIndexBuffer(
        encoded.data() + vertexBytes, encoded.size() - vertexBytes, mesh.Indices.data(), mesh.Indices.size());
    if (indexBytes == 0)
        return false;
    page.EncodedVertexBytes = static_cast<uint32>(vertexBytes);
    page.Bytes = vertexBytes + indexBytes;
    return writer.Bytes(encoded.data(), static_cast<size_t>(page.Bytes));
}

bool WritePage(Writer& writer, const MeshClusterLODManifest::Page& page)
{
    return writer.Value(page.Offset) && writer.Value(page.Bytes) && writer.Value(page.ContentHash) &&
        writer.Value(page.EncodedVertexBytes) && writer.Value(page.VertexCount) && writer.Value(page.IndexCount) &&
        writer.Value(page.Error) &&
        writer.Value(page.CoarseFirstIndex) && writer.Value(page.CoarseIndexCount) &&
        writer.Value(page.MinBounds) && writer.Value(page.MaxBounds);
}

// The page's coarse range follows the previous page's range in the same mesh.
bool ReadPage(Reader& reader, uint64 storeBytes, uint64 expectedOffset, uint32 expectedFirstIndex,
              MeshClusterLODManifest::Page& page)
{
    if (!reader.Value(page.Offset) || !reader.Value(page.Bytes) || !reader.Value(page.ContentHash) ||
        !reader.Value(page.EncodedVertexBytes) || !reader.Value(page.VertexCount) || !reader.Value(page.IndexCount) ||
        !reader.Value(page.Error) ||
        !reader.Value(page.CoarseFirstIndex) || !reader.Value(page.CoarseIndexCount) ||
        !reader.Value(page.MinBounds) || !reader.Value(page.MaxBounds))
        return false;
    const std::array<float, 6> bounds{page.MinBounds.x, page.MinBounds.y, page.MinBounds.z,
                                      page.MaxBounds.x, page.MaxBounds.y, page.MaxBounds.z};
    if (!ValidFloats(bounds) || bounds[0] > bounds[3] || bounds[1] > bounds[4] || bounds[2] > bounds[5] ||
        page.CoarseFirstIndex != expectedFirstIndex || page.CoarseIndexCount == 0 || page.CoarseIndexCount % 3 != 0)
        return false;
    return page.Offset == expectedOffset && page.Offset <= storeBytes &&
        page.Bytes <= storeBytes - page.Offset && page.VertexCount != 0 && page.VertexCount <= kMaximumPageVertices &&
        page.IndexCount != 0 && page.IndexCount % 3 == 0 &&
        static_cast<uint64>(page.VertexCount) * sizeof(Vertex) + static_cast<uint64>(page.IndexCount) * sizeof(uint32) <=
            kMaximumFileBytes &&
        page.EncodedVertexBytes != 0 && page.EncodedVertexBytes < page.Bytes &&
        page.EncodedVertexBytes <= meshopt_encodeVertexBufferBound(page.VertexCount, sizeof(Vertex)) &&
        page.Bytes <= EncodedPageBound(page.VertexCount, page.IndexCount) &&
        std::isfinite(page.Error) && page.Error >= 0.0f;
}

// The mesh-local bounds of a page cover its coarse and its fine vertices.
void SetPageBounds(const MeshClusterLODManifest::Page& page, Mesh& mesh)
{
    for (size_t axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = page.MinBounds[axis];
        mesh.MaxBounds[axis] = page.MaxBounds[axis];
    }
}

// Concatenates the coarse groups in order, so each group is one contiguous index range, then
// welds equal vertices. Group borders are locked, so the welded mesh has no seams between groups.
Mesh BuildCoarseMesh(const Mesh& source, const Vector<MeshClusterLODGroup>& groups)
{
    Mesh coarse;
    CopyMeshClusterLODPlacement(source, coarse);
    std::fill_n(coarse.MinBounds, 3, std::numeric_limits<float>::max());
    std::fill_n(coarse.MaxBounds, 3, std::numeric_limits<float>::lowest());
    Vector<Vertex> vertices;
    for (const MeshClusterLODGroup& group : groups)
    {
        const auto base = static_cast<uint32>(vertices.size());
        vertices.insert(vertices.end(), group.Coarse.Vertices.begin(), group.Coarse.Vertices.end());
        for (const uint32 index : group.Coarse.Indices)
            coarse.Indices.push_back(base + index);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            coarse.MinBounds[axis] = std::min(coarse.MinBounds[axis], group.Coarse.MinBounds[axis]);
            coarse.MaxBounds[axis] = std::max(coarse.MaxBounds[axis], group.Coarse.MaxBounds[axis]);
        }
    }
    Vector<uint32> remap(vertices.size());
    const size_t unique = meshopt_generateVertexRemap(remap.data(), coarse.Indices.data(), coarse.Indices.size(),
                                                      vertices.data(), vertices.size(), sizeof(Vertex));
    coarse.Vertices.resize(unique);
    meshopt_remapVertexBuffer(coarse.Vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
    meshopt_remapIndexBuffer(coarse.Indices.data(), coarse.Indices.data(), coarse.Indices.size(), remap.data());
    return coarse;
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
    if (!reader.Value(magic) || magic != kMagic || !reader.Value(version) || version != kVersion ||
        !reader.Value(decoded.Manifest.Incarnation) ||
        Hashing::Fnv1a64(bytes.data() + bytes.size() - reader.Remaining(), reader.Remaining()) != decoded.Manifest.Incarnation)
        return false;
    if (!reader.Value(decoded.Manifest.StoreHash) || !reader.Value(decoded.Manifest.StoreBytes) ||
        decoded.Manifest.StoreBytes == 0 || decoded.Manifest.StoreBytes > kMaximumFileBytes)
    {
        error = "Cluster fine store size is missing or exceeds its limit";
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
    uint64 offset = 0;
    uint32 placements = 0;
    FlatSet<String> names;
    names.Reserve(meshes);
    for (uint32 meshIndex = 0; meshIndex < meshes; ++meshIndex)
    {
        Mesh& mesh = decoded.CoarseMeshes[meshIndex];
        uint32 vertices = 0;
        uint32 indices = 0;
        if (!ReadPlacement(reader, mesh, placements) || !names.Insert(mesh.Name) ||
            (!decoded.Materials.empty() && mesh.MaterialIndex >= decoded.Materials.size()) ||
            (decoded.Materials.empty() && mesh.MaterialIndex != 0))
        {
            error = std::format("Cluster mesh {} has invalid placement, bounds, name, or material", meshIndex);
            return false;
        }
        if (!reader.Value(vertices) || !reader.Value(indices) ||
            !ReadGeometry(reader, vertices, indices, kMaximumCoarseVertices, mesh))
        {
            error = std::format("Cluster mesh {} has invalid coarse geometry", meshIndex);
            return false;
        }
        uint32 groups = 0;
        if (!reader.Value(groups) || groups == 0 || groups > kMaximumGroups - decoded.Manifest.Pages.size())
            return false;
        uint32 firstIndex = 0;
        for (uint32 group = 0; group < groups; ++group)
        {
            MeshClusterLODManifest::Page page;
            if (!ReadPage(reader, decoded.Manifest.StoreBytes, offset, firstIndex, page) ||
                page.CoarseIndexCount > indices - firstIndex)
            {
                error = std::format("Cluster mesh {} group {} has an invalid fine page or coarse range", meshIndex, group);
                return false;
            }
            page.Mesh = meshIndex;
            for (uint32 index = page.CoarseFirstIndex; index < page.CoarseFirstIndex + page.CoarseIndexCount; ++index)
            {
                const auto& position = mesh.Vertices[mesh.Indices[index]].Position;
                if (position[0] < page.MinBounds.x || position[0] > page.MaxBounds.x ||
                    position[1] < page.MinBounds.y || position[1] > page.MaxBounds.y ||
                    position[2] < page.MinBounds.z || position[2] > page.MaxBounds.z)
                {
                    error = std::format("Cluster mesh {} group {} has coarse vertices outside its bounds", meshIndex, group);
                    return false;
                }
            }
            firstIndex += page.CoarseIndexCount;
            offset += page.Bytes;
            decoded.Manifest.Pages.push_back(page);
        }
        if (firstIndex != indices)
        {
            error = std::format("Cluster mesh {} groups do not cover its coarse indices", meshIndex);
            return false;
        }
    }
    if (offset != decoded.Manifest.StoreBytes || reader.Remaining() != 0 || IsAssetDecodeCancelled())
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
    decoded.Manifest.TextureDependencies.assign(dependencies.begin(), dependencies.end());
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
    Writer groups;
    FlatSet<String> names;
    uint32 meshCount = 0;
    uint32 count = 0;
    uint32 placements = 0;
    uint64 geometryBytes = 0;
    uint64 metadataBytes = 0;
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
        Vector<MeshClusterLODGroup> generated;
        if (!GenerateMeshClusterLODs(mesh, config, generated, error))
            return false;
        if (generated.size() > config.MaximumGroups - count || placements >= config.MaximumGroups ||
            mesh.ExtraPlacements.size() > config.MaximumGroups - placements - 1)
        {
            error = std::format("Cluster model exceeds {} placed groups", config.MaximumGroups);
            return false;
        }
        placements += static_cast<uint32>(mesh.ExtraPlacements.size()) + 1;
        Mesh coarse = BuildCoarseMesh(mesh, generated);
        if (!names.Insert(coarse.Name))
            coarse.Name = std::format("{} {}", mesh.Name, meshCount);
        if (coarse.Name != mesh.Name && !names.Insert(coarse.Name))
        {
            error = "Cluster mesh names are not unique";
            return false;
        }
        const uint64 meshMetadata = sizeof(Mesh) + coarse.Name.size() + generated.size() * sizeof(MeshClusterLODManifest::Page) +
            coarse.ExtraPlacements.size() * sizeof(MeshPlacement);
        if (meshMetadata > config.MaximumMetadataBytes - metadataBytes)
        {
            error = "Cluster model exceeds the group metadata limit";
            return false;
        }
        metadataBytes += meshMetadata;
        const uint64 coarseBytes = coarse.Vertices.size() * sizeof(Vertex) + coarse.Indices.size() * sizeof(uint32);
        if (coarseBytes > config.MaximumGeometryBytes - geometryBytes)
        {
            error = "Cluster model exceeds the total geometry payload limit";
            return false;
        }
        geometryBytes += coarseBytes;
        if (!WritePlacement(groups, coarse) || !groups.Value(static_cast<uint32>(coarse.Vertices.size())) ||
            !groups.Value(static_cast<uint32>(coarse.Indices.size())) || !WriteGeometry(groups, coarse) ||
            !groups.Value(static_cast<uint32>(generated.size())))
        {
            error = "Cluster groups exceed their serialized file limit";
            return false;
        }
        uint32 firstIndex = 0;
        for (const MeshClusterLODGroup& group : generated)
        {
            const uint64 fineBytes = group.Fine.Vertices.size() * sizeof(Vertex) + group.Fine.Indices.size() * sizeof(uint32);
            if (fineBytes > config.MaximumGeometryBytes - geometryBytes)
            {
                error = "Cluster model exceeds the total geometry payload limit";
                return false;
            }
            geometryBytes += fineBytes;
            MeshClusterLODManifest::Page page;
            page.Offset = fine.Data().size();
            page.VertexCount = static_cast<uint32>(group.Fine.Vertices.size());
            page.IndexCount = static_cast<uint32>(group.Fine.Indices.size());
            page.Error = group.Error;
            page.CoarseFirstIndex = firstIndex;
            page.CoarseIndexCount = static_cast<uint32>(group.Coarse.Indices.size());
            page.MinBounds = {group.Coarse.MinBounds[0], group.Coarse.MinBounds[1], group.Coarse.MinBounds[2]};
            page.MaxBounds = {group.Coarse.MaxBounds[0], group.Coarse.MaxBounds[1], group.Coarse.MaxBounds[2]};
            firstIndex += page.CoarseIndexCount;
            if (!WriteEncodedPage(fine, group.Fine, page))
            {
                error = "Cluster fine store exceeds its serialized file limit";
                return false;
            }
            const auto payload = fine.Data().subspan(static_cast<size_t>(page.Offset), static_cast<size_t>(page.Bytes));
            page.ContentHash = Hashing::Fnv1a64(payload.data(), payload.size());
            if (!WritePage(groups, page))
            {
                error = "Cluster groups exceed their serialized file limit";
                return false;
            }
            ++count;
        }
        ++meshCount;
    }
    const uint64 storeHash = Hashing::Fnv1a64(fine.Data().data(), fine.Data().size());
    Writer body;
    if (!body.Value(storeHash) || !body.Value(static_cast<uint64>(fine.Data().size())) ||
        !body.Value(static_cast<uint32>(materialText.size())) || !body.Bytes(materialText.data(), materialText.size()) ||
        !WriteImages(body, source.GetEmbeddedImages()) || !body.Value(meshCount) || !body.Bytes(groups.Data().data(), groups.Data().size()))
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

bool DecodeMeshClusterLODFinePage(std::span<const uint8> bytes, const MeshClusterLODManifest::Page& page,
                                 const Mesh& coarse, Mesh& output, String& error)
{
    error.clear();
    if (bytes.size() > kMaximumFileBytes || bytes.size() != page.Bytes || page.EncodedVertexBytes >= bytes.size() ||
        Hashing::Fnv1a64(bytes.data(), bytes.size()) != page.ContentHash)
    {
        error = "Cluster fine page is truncated or has the wrong checksum";
        return false;
    }
    const auto vertexStream = bytes.first(page.EncodedVertexBytes);
    const auto indexStream = bytes.subspan(page.EncodedVertexBytes);
    Mesh decoded;
    CopyMeshClusterLODPlacement(coarse, decoded);
    SetPageBounds(page, decoded);
    decoded.Vertices.resize(page.VertexCount);
    decoded.Indices.resize(page.IndexCount);
    // The decoders accept untrusted input; ValidGeometry refuses garbage output.
    if (meshopt_decodeVertexBuffer(decoded.Vertices.data(), page.VertexCount, sizeof(Vertex), vertexStream.data(),
                                   vertexStream.size()) != 0 ||
        meshopt_decodeIndexBuffer(decoded.Indices.data(), page.IndexCount, sizeof(uint32), indexStream.data(),
                                  indexStream.size()) != 0 ||
        !ValidGeometry(decoded) || IsAssetDecodeCancelled())
    {
        error = "Cluster fine page has invalid geometry or was cancelled";
        return false;
    }
    output = std::move(decoded);
    return true;
}

Mesh ExtractMeshClusterLODCoarseGroup(const Mesh& coarse, const MeshClusterLODManifest::Page& page)
{
    Vector<uint32> remap(coarse.Vertices.size(), kMeshClusterLODUnusedVertex);
    Vector<uint32> sourceVertices;
    Mesh group = CompactMeshClusterLODTriangles(
        coarse, std::span<const uint32>(coarse.Indices.data() + page.CoarseFirstIndex, page.CoarseIndexCount), remap,
        sourceVertices);
    SetPageBounds(page, group);
    return group;
}

float MeshClusterLODRelativeCoarseError(const MeshClusterLODManifest& manifest, uint32 mesh, const Mesh& coarse)
{
    float error = 0.0f;
    for (const MeshClusterLODManifest::Page& page : manifest.Pages)
        if (page.Mesh == mesh)
            error = std::max(error, page.Error);
    const float extent = std::max({coarse.MaxBounds[0] - coarse.MinBounds[0], coarse.MaxBounds[1] - coarse.MinBounds[1],
                                   coarse.MaxBounds[2] - coarse.MinBounds[2]});
    return extent > 0.0f ? error / extent : 0.0f;
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
