#include "Ocean/OceanCpuTextureCache.h"

#include "Assets/TextureAsset.h"
#include "Mathematics/HalfFloat.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine::Ocean
{

namespace
{
float SrgbChannelToLinear(float value)
{
    value = Math::Clamp01(value);
    if (value <= 0.04045f)
        return value / 12.92f;
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// One channel of one texel as the GPU sampler returns it: normalized, and linear for sRGB color.
float ReadTextureChannel01(const uint8* pixels, uint32 channels, size_t pixelIndex, uint32 channel,
                           uint32 bytesPerChannel, bool decodeSrgb)
{
    if (channels == 0u || channel >= channels)
        return 1.0f;

    const size_t channelIndex = pixelIndex * channels + channel;
    float value = 1.0f;
    if (bytesPerChannel == 1u)
    {
        value = static_cast<float>(pixels[channelIndex]) / 255.0f;
    }
    else if (bytesPerChannel == 2u)
    {
        const auto* halfs = reinterpret_cast<const uint16*>(pixels);
        value = Mathematics::HalfToFloat(halfs[channelIndex]);
    }
    else if (bytesPerChannel == 4u)
    {
        const auto* floats = reinterpret_cast<const float*>(pixels);
        value = floats[channelIndex];
    }

    if (decodeSrgb && channel < 3u)
        value = SrgbChannelToLinear(value);
    return value;
}
} // namespace

OceanCpuTextureRG OceanCpuTextureCache::Convert(const TextureAsset& texture)
{
    OceanCpuTextureRG out{};
    const uint32 width = texture.GetWidth();
    const uint32 height = texture.GetHeight();
    const uint32 channels = texture.GetChannels();
    // The asset can be the instance that the AssetManager shares: a material mip stream releases
    // its pixels. The held pixels stay valid for this read.
    const std::shared_ptr<const uint8[]> heldPixels = texture.AcquirePixels();
    const uint8* pixels = heldPixels.get();
    if (!pixels || width == 0u || height == 0u || channels == 0u)
        return out;

    const uint64 texelCount = static_cast<uint64>(width) * height;
    const uint64 minBytes = texelCount * channels;
    if (texture.GetDataSize() < minBytes)
        return out;
    const uint32 bytesPerChannel =
        static_cast<uint32>(std::max<uint64>(1u, texture.GetDataSize() / minBytes));
    if (bytesPerChannel != 1u && bytesPerChannel != 2u && bytesPerChannel != 4u)
        return out;

    out.Width = width;
    out.Height = height;
    out.RG.resize(static_cast<size_t>(texelCount) * 2u);

    const bool decodeSrgb = texture.GetColorSpace() == TextureColorSpace::SRGB &&
                            bytesPerChannel == 1u;
    for (uint64 texel = 0u; texel < texelCount; ++texel)
    {
        out.RG[static_cast<size_t>(texel) * 2u + 0u] =
            ReadTextureChannel01(pixels, channels, static_cast<size_t>(texel), 0u, bytesPerChannel, decodeSrgb);
        out.RG[static_cast<size_t>(texel) * 2u + 1u] =
            ReadTextureChannel01(pixels, channels, static_cast<size_t>(texel), 1u, bytesPerChannel, decodeSrgb);
    }
    return out;
}

const OceanCpuTextureRG& OceanCpuTextureCache::Get(const GUID& guid, const TextureAsset& texture)
{
    const uint32 generation = texture.GetPayloadGeneration();
    Entry* entry = m_Entries.Find(guid);
    if (!entry || entry->Texture != &texture || entry->PayloadGeneration != generation)
    {
        m_Entries.InsertOrAssign(guid, Entry{&texture, generation, false, Convert(texture)});
        entry = m_Entries.Find(guid);
    }
    entry->Used = true;
    return entry->Copy;
}

void OceanCpuTextureCache::DropUnused()
{
    m_Entries.EraseIf([](const GUID&, const Entry& entry) { return !entry.Used; });
    for (auto& [guid, entry] : m_Entries)
        entry.Used = false;
}

} // namespace GameEngine::Ocean
