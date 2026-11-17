#pragma once

// In-memory glTF binary files for tests: the GLB container, a clip of one channel whose animation extras the
// test chooses, and a morph target mesh whose clip animates its weights, read the way the importer reads a file
// (AnimationClip::LoadFromData, ModelAsset's glTF loader).

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::TestFiles
{

// Appends `value` to `bytes` in the machine's byte order, little-endian on every platform the engine builds for,
// which is the order a GLB header and chunk header use.
inline void AppendUint32(std::string& bytes, uint32_t value)
{
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// A GLB container of `json` and `bin`: the 12-byte header, the JSON chunk padded with spaces and the BIN
// chunk padded with zeros, each to four bytes.
inline std::string BinaryGltf(std::string json, std::string bin)
{
    constexpr size_t kChunkAlignment = 4u;
    constexpr size_t kHeaderBytes = 12u;         // magic, version, total length
    constexpr size_t kChunkHeaderBytes = 8u;     // chunk length, chunk type
    constexpr uint32_t kGlbVersion = 2u;
    constexpr uint32_t kJsonChunk = 0x4E4F534Au; // "JSON"
    constexpr uint32_t kBinChunk = 0x004E4942u;  // "BIN\0"
    json.append((kChunkAlignment - json.size() % kChunkAlignment) % kChunkAlignment, ' ');
    bin.append((kChunkAlignment - bin.size() % kChunkAlignment) % kChunkAlignment, '\0');
    std::string glb = "glTF";
    AppendUint32(glb, kGlbVersion);
    AppendUint32(glb, static_cast<uint32_t>(kHeaderBytes + kChunkHeaderBytes + json.size() + kChunkHeaderBytes +
                                            bin.size()));
    AppendUint32(glb, static_cast<uint32_t>(json.size()));
    AppendUint32(glb, kJsonChunk);
    glb += json;
    AppendUint32(glb, static_cast<uint32_t>(bin.size()));
    AppendUint32(glb, kBinChunk);
    glb += bin;
    return glb;
}

// A GLB whose animation "Swing" translates node "Mover" over `keyCount` keys `1 / framesPerSecond` seconds apart, so
// the clip is sampled at `framesPerSecond` and lasts (keyCount - 1) / framesPerSecond seconds, with `extras` as the
// animation's extras.
inline std::string ClipGlb(size_t keyCount, float framesPerSecond, std::string_view extras)
{
    std::string bin;
    for (size_t key = 0; key < keyCount; ++key)
    {
        const float time = static_cast<float>(key) / framesPerSecond;
        bin.append(reinterpret_cast<const char*>(&time), sizeof(time));
    }
    for (size_t key = 0; key < keyCount; ++key)
    {
        const float translation[3] = {static_cast<float>(key) / framesPerSecond, 0.0f, 0.0f};
        bin.append(reinterpret_cast<const char*>(translation), sizeof(translation));
    }
    const size_t timeBytes = keyCount * sizeof(float);
    const size_t translationBytes = keyCount * 3u * sizeof(float);
    const std::string count = std::to_string(keyCount);
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Mover"}],)"
                    R"("animations":[{"name":"Swing","samplers":[{"input":0,"output":1,"interpolation":"LINEAR"}],)"
                    R"("channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}],"extras":)") +
        std::string(extras) + R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":)" + count +
        R"(,"type":"SCALAR","min":[0],"max":[)" +
        std::format("{}", static_cast<float>(keyCount - 1u) / framesPerSecond) +
        R"(]},{"bufferView":1,"componentType":5126,"count":)" + count + R"(,"type":"VEC3"}],)" +
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(timeBytes) +
        R"(},{"buffer":0,"byteOffset":)" + std::to_string(timeBytes) + R"(,"byteLength":)" +
        std::to_string(translationBytes) + R"(}],"buffers":[{"byteLength":)" +
        std::to_string(timeBytes + translationBytes) + R"(}]})";
    return BinaryGltf(json, bin);
}

// Appends `values` to `bytes` as 32-bit floats in the machine's byte order (little-endian, as in a GLB BIN chunk).
inline void AppendFloats(std::string& bytes, const std::vector<float>& values)
{
    bytes.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float));
}

// The weights of the "Talk" clip of MorphWeightsGlb at each of its key times 0, 1 and 2 seconds: { Smile, Blink }.
inline constexpr float kTalkWeights[3][2] = {{0.0f, 0.0f}, {1.0f, 0.5f}, {0.0f, 1.0f}};

// A GLB with node "Face" drawing the triangle (0, 1, 0), (-1, -1, 0), (1, -1, 0) with two morph targets,
// extras.targetNames "Smile" (vertex 0 up by 2) and "Blink" (vertex 1 out along -X by 2). Animation "Talk"
// drives the node's weights with kTalkWeights. A CUBICSPLINE sampler stores in-tangents of 7 and out-tangents
// of 9 around each key's weights, values no key holds.
inline std::string MorphWeightsGlb(bool cubicSpline)
{
    constexpr size_t kKeys = 3u;
    constexpr size_t kTargets = 2u;
    constexpr float kInTangent = 7.0f;
    constexpr float kOutTangent = 9.0f;
    std::string bin;
    AppendFloats(bin, {0.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f});
    AppendFloats(bin, {0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    AppendFloats(bin, {0.0f, 0.0f, 0.0f, -2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    AppendFloats(bin, {0.0f, 1.0f, 2.0f});
    for (const auto& weights : kTalkWeights)
    {
        if (cubicSpline)
            AppendFloats(bin, {kInTangent, kInTangent});
        AppendFloats(bin, {weights[0], weights[1]});
        if (cubicSpline)
            AppendFloats(bin, {kOutTangent, kOutTangent});
    }
    constexpr size_t kVec3Bytes = 3u * 3u * sizeof(float);
    constexpr size_t kTimeBytes = kKeys * sizeof(float);
    const size_t weightCount = kKeys * kTargets * (cubicSpline ? 3u : 1u);
    const std::string json =
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
                    R"("nodes":[{"name":"Face","mesh":0}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"targets":[{"POSITION":1},{"POSITION":2}]}],)"
                    R"("weights":[0,0],"extras":{"targetNames":["Smile","Blink"]}}],)"
                    R"("animations":[{"name":"Talk","samplers":[{"input":3,"output":4,"interpolation":")") +
        (cubicSpline ? "CUBICSPLINE" : "LINEAR") +
        R"("}],"channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,2,0]},)"
        R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[-2,0,0],"max":[0,0,0]},)"
        R"({"bufferView":3,"componentType":5126,"count":3,"type":"SCALAR","min":[0],"max":[2]},)"
        R"({"bufferView":4,"componentType":5126,"count":)" + std::to_string(weightCount) + R"(,"type":"SCALAR"}],)" +
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(kVec3Bytes) + "}," +
        R"({"buffer":0,"byteOffset":)" + std::to_string(kVec3Bytes) + R"(,"byteLength":)" +
        std::to_string(kVec3Bytes) + "}," + R"({"buffer":0,"byteOffset":)" + std::to_string(2u * kVec3Bytes) +
        R"(,"byteLength":)" + std::to_string(kVec3Bytes) + "}," + R"({"buffer":0,"byteOffset":)" +
        std::to_string(3u * kVec3Bytes) + R"(,"byteLength":)" + std::to_string(kTimeBytes) + "}," +
        R"({"buffer":0,"byteOffset":)" + std::to_string(3u * kVec3Bytes + kTimeBytes) + R"(,"byteLength":)" +
        std::to_string(weightCount * sizeof(float)) + R"(}],"buffers":[{"byteLength":)" +
        std::to_string(bin.size()) + R"(}]})";
    return BinaryGltf(json, bin);
}

} // namespace GameEngine::TestFiles
