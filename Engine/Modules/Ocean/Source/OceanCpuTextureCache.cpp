#include "Ocean/OceanCpuTextureCache.h"

#include "Assets/TextureAsset.h"
#include "Mathematics/HalfFloat.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace GameEngine::Ocean
{

namespace
{
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
        value = Detail::SRGBChannelToLinear(value);
    return value;
}
} // namespace

OceanCpuTextureRG OceanCpuTextureCache::Convert(const TexturePixels& texture)
{
    OceanCpuTextureRG out{};
    const uint8* pixels = texture.Pixels.get();
    if (!pixels || texture.Width == 0u || texture.Height == 0u || texture.Channels == 0u)
        return out;

    const uint64 texelCount = static_cast<uint64>(texture.Width) * texture.Height;
    const uint64 minBytes = texelCount * texture.Channels;
    if (texture.DataSize < minBytes)
        return out;
    const uint32 bytesPerChannel = static_cast<uint32>(std::max<uint64>(1u, texture.DataSize / minBytes));
    if (bytesPerChannel != 1u && bytesPerChannel != 2u && bytesPerChannel != 4u)
        return out;

    out.Width = texture.Width;
    out.Height = texture.Height;
    out.RG.resize(static_cast<size_t>(texelCount) * 2u);

    const bool decodeSrgb = texture.ColorSpace == TextureColorSpace::SRGB && bytesPerChannel == 1u;
    for (uint64 texel = 0u; texel < texelCount; ++texel)
    {
        out.RG[static_cast<size_t>(texel) * 2u + 0u] = ReadTextureChannel01(
            pixels, texture.Channels, static_cast<size_t>(texel), 0u, bytesPerChannel, decodeSrgb);
        out.RG[static_cast<size_t>(texel) * 2u + 1u] = ReadTextureChannel01(
            pixels, texture.Channels, static_cast<size_t>(texel), 1u, bytesPerChannel, decodeSrgb);
    }
    return out;
}

std::shared_ptr<const OceanCpuTextureRG> OceanCpuTextureCache::Get(const GUID& guid, const TextureAsset& texture)
{
    if (Entry* entry = m_Entries.Find(guid); entry && entry->PayloadGeneration == texture.GetPayloadGeneration())
    {
        entry->Used = true;
        return entry->Copy;
    }
    // The pixels, the dimensions and the generation are of one payload: a reload during the
    // conversion gives a copy of the old payload under the old generation.
    const TexturePixels pixels = texture.AcquirePixels();
    OceanCpuTextureRG converted = Convert(pixels);
    if (!converted.IsValid())
    {
        m_Entries.Erase(guid);
        return nullptr;
    }
    auto copy = std::make_shared<const OceanCpuTextureRG>(std::move(converted));
    m_Entries.InsertOrAssign(guid, Entry{pixels.PayloadGeneration, true, copy});
    return copy;
}

void OceanCpuTextureCache::DropUnused()
{
    m_Entries.EraseIf([](const GUID&, const Entry& entry) { return !entry.Used; });
    for (auto& [guid, entry] : m_Entries)
        entry.Used = false;
}

} // namespace GameEngine::Ocean
