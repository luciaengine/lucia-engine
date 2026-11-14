#include "Animation/Compression/AnimationClipArchive.h"

#include <algorithm>
#include <array>

#include <zlib.h>

namespace GameEngine::Animation
{
namespace
{
constexpr std::array<uint8, 4> kMagic{'L', 'C', 'A', '1'};
constexpr size_t kHeaderBytes = 12u;
constexpr uint32 kVersion = 1u;

void AppendWord(std::vector<uint8>& bytes, uint32 value)
{
    for (uint32 shift = 0u; shift < 32u; shift += 8u)
        bytes.push_back(static_cast<uint8>(value >> shift));
}

uint32 ReadWord(std::span<const uint8> bytes, size_t offset)
{
    uint32 value = 0u;
    for (uint32 index = 0u; index < 4u; ++index)
        value |= static_cast<uint32>(bytes[offset + index]) << (index * 8u);
    return value;
}
}

bool AnimationClipArchive::IsArchive(std::span<const uint8> bytes)
{
    return bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

size_t AnimationClipArchive::MaximumEncodedBytes()
{
    return kHeaderBytes + compressBound(kMaximumSourceBytes);
}

bool AnimationClipArchive::Encode(std::span<const uint8> source, std::vector<uint8>& output)
{
    if (source.size() > kMaximumSourceBytes)
        return false;
    uLongf capacity = compressBound(static_cast<uLong>(source.size()));
    std::vector<uint8> encoded;
    encoded.insert(encoded.end(), kMagic.begin(), kMagic.end());
    AppendWord(encoded, kVersion);
    AppendWord(encoded, static_cast<uint32>(source.size()));
    encoded.resize(kHeaderBytes + capacity);
    if (compress2(encoded.data() + kHeaderBytes, &capacity, source.data(),
            static_cast<uLong>(source.size()), Z_BEST_SPEED) != Z_OK)
        return false;
    encoded.resize(kHeaderBytes + capacity);
    // The scratch bound is much larger than the compressed payload.
    output = std::vector<uint8>(encoded.begin(), encoded.end());
    return true;
}

bool AnimationClipArchive::Decode(std::span<const uint8> bytes, std::vector<uint8>& output)
{
    if (bytes.size() < kHeaderBytes || !IsArchive(bytes) || ReadWord(bytes, 4u) != kVersion)
        return false;
    const uint32 sourceBytes = ReadWord(bytes, 8u);
    if (sourceBytes > kMaximumSourceBytes ||
        bytes.size() - kHeaderBytes > compressBound(kMaximumSourceBytes))
        return false;
    // One spare byte lets inflate finish an empty stream without a null output buffer.
    std::vector<uint8> decoded(static_cast<size_t>(sourceBytes) + 1u);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(bytes.data() + kHeaderBytes);
    stream.avail_in = static_cast<uInt>(bytes.size() - kHeaderBytes);
    stream.next_out = decoded.data();
    stream.avail_out = static_cast<uInt>(decoded.size());
    if (inflateInit(&stream) != Z_OK)
        return false;
    const int result = inflate(&stream, Z_FINISH);
    const bool valid = result == Z_STREAM_END && stream.total_out == sourceBytes && stream.avail_in == 0u;
    inflateEnd(&stream);
    if (!valid)
        return false;
    decoded.resize(sourceBytes);
    output = std::move(decoded);
    return true;
}

} // namespace GameEngine::Animation
